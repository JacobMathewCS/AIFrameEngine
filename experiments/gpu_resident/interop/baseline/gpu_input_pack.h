#pragma once
#include <d3d12.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <array>
#include <cstring>
#include <stdexcept>
#include <string>

// Scratch candidate: one immutable input job, one recording. No ORT or CPU image
// readback. Caller owns submission/fences and retains this object until completion.
// Input must be a completed BGRA8 UNORM resize, in COMMON, on this D3D12 device.
// Output is float32 RGB NCHW in UAV state. Bind it on the SAME ordered queue later.
// This intentionally does not fuse resize: preserve the baseline UNORM quantization.
class GpuInputPack {
    template<class T> using Ptr = Microsoft::WRL::ComPtr<T>;
public:
    static void Check(HRESULT hr, const char* message) {
        if (FAILED(hr)) throw std::runtime_error(std::string(message) + " HRESULT=" +
            std::to_string(static_cast<unsigned long>(hr)));
    }
    GpuInputPack(ID3D12Device* device, ID3D12Resource* input) : device_(device), input_(input) {
        if (!device || !input) throw std::invalid_argument("null input/device");
        Ptr<ID3D12Device> owner;
        Check(input->GetDevice(IID_PPV_ARGS(&owner)), "input device");
        if (owner.Get() != device) throw std::invalid_argument("input device mismatch");
        const auto d = input->GetDesc();
        if (d.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
            d.Format != DXGI_FORMAT_B8G8R8A8_UNORM || d.DepthOrArraySize != 1 ||
            d.MipLevels != 1 || d.SampleDesc.Count != 1 || !d.Width || !d.Height ||
            d.Width > 16384 || d.Height > 16384 || (d.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE))
            throw std::invalid_argument("expected single BGRA8 UNORM texture");
        width_ = static_cast<UINT>(d.Width); height_ = d.Height;
        bytes_ = static_cast<UINT64>(width_) * height_ * 3 * sizeof(float);
        output_ = Buffer(device, bytes_, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        // One-time 1 KiB normalization table, not image data. Use the same CPU
        // byte/255.f expression as PackRgb to preserve bit patterns on this build.
        // A table avoids compiler-dependent shader division/reciprocal rounding.
        normalization_ = Buffer(device, 1024, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
        std::array<float,256> table{};
        for (size_t i = 0; i < table.size(); ++i) table[i] = static_cast<float>(i) / 255.0f;
        void* mapped = nullptr; D3D12_RANGE empty{0,0};
        Check(normalization_->Map(0, &empty, &mapped), "normalization map");
        std::memcpy(mapped, table.data(), sizeof(table));
        D3D12_RANGE written{0, sizeof(table)}; normalization_->Unmap(0, &written);

        static const char code[] = R"(
Texture2D<float4> image : register(t0);
RWByteAddressBuffer tensor : register(u0);
cbuffer Shape : register(b0) { uint width; uint height; };
cbuffer Normalization : register(b1) { float4 normalized[64]; };
float value(float channel) {
    uint b = (uint)round(saturate(channel) * 255.0f);
    return normalized[b >> 2][b & 3];
}
[numthreads(16,16,1)] void main(uint3 p : SV_DispatchThreadID) {
    if (p.x >= width || p.y >= height) return;
    // BGRA format swizzle is performed by the typed SRV: x/y/z are R/G/B.
    float4 pixel = image.Load(int3(p.xy,0));
    uint offset = 4 * (p.y * width + p.x), plane = 4 * width * height;
    tensor.Store(offset, asuint(value(pixel.x)));
    tensor.Store(offset + plane, asuint(value(pixel.y)));
    tensor.Store(offset + 2 * plane, asuint(value(pixel.z)));
})";
        Ptr<ID3DBlob> shader, errors, signature;
        const HRESULT compiled = D3DCompile(code, sizeof(code)-1, nullptr, nullptr, nullptr,
            "main", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3 | D3DCOMPILE_WARNINGS_ARE_ERRORS,
            0, &shader, &errors);
        if (FAILED(compiled) && errors)
            throw std::runtime_error(std::string(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize()));
        Check(compiled, "compile input packing shader");
        D3D12_DESCRIPTOR_RANGE range{};
        range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; range.NumDescriptors = 1;
        D3D12_ROOT_PARAMETER params[4]{};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[0].DescriptorTable = {1, &range};
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[2].Constants = {0, 0, 2};
        params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        params[3].Descriptor.ShaderRegister = 1;
        D3D12_ROOT_SIGNATURE_DESC rootDesc{};
        rootDesc.NumParameters = 4; rootDesc.pParameters = params;
        Check(D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &errors), "serialize pack root");
        Check(device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(),
            IID_PPV_ARGS(&root_)), "pack root");
        D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline{};
        pipeline.pRootSignature = root_.Get(); pipeline.CS = {shader->GetBufferPointer(), shader->GetBufferSize()};
        Check(device->CreateComputePipelineState(&pipeline, IID_PPV_ARGS(&pipeline_)), "pack pipeline");
        D3D12_DESCRIPTOR_HEAP_DESC heap{};
        heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; heap.NumDescriptors = 1;
        heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        Check(device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&descriptors_)), "pack descriptor heap");
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = d.Format; srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MipLevels = 1;
        device->CreateShaderResourceView(input, &srv, descriptors_->GetCPUDescriptorHandleForHeapStart());
    }
    GpuInputPack(const GpuInputPack&) = delete;
    GpuInputPack& operator=(const GpuInputPack&) = delete;
    void Record(ID3D12GraphicsCommandList* list) {
        if (!list || recorded_) throw std::logic_error("null list or repeated input record");
        Ptr<ID3D12Device> owner;
        Check(list->GetDevice(IID_PPV_ARGS(&owner)), "list device");
        if (owner.Get() != device_.Get() || list->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT)
            throw std::invalid_argument("pack requires same-device DIRECT list");
        recorded_ = true;
        Transition(list, input_.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        list->SetPipelineState(pipeline_.Get()); list->SetComputeRootSignature(root_.Get());
        ID3D12DescriptorHeap* heaps[]{descriptors_.Get()}; list->SetDescriptorHeaps(1, heaps);
        list->SetComputeRootDescriptorTable(0, descriptors_->GetGPUDescriptorHandleForHeapStart());
        list->SetComputeRootUnorderedAccessView(1, output_->GetGPUVirtualAddress());
        const UINT shape[]{width_, height_}; list->SetComputeRoot32BitConstants(2, 2, shape, 0);
        list->SetComputeRootConstantBufferView(3, normalization_->GetGPUVirtualAddress());
        list->Dispatch((width_+15)/16, (height_+15)/16, 1);
        D3D12_RESOURCE_BARRIER uav{}; uav.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        uav.UAV.pResource = output_.Get(); list->ResourceBarrier(1, &uav);
        Transition(list, input_.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
    }
    ID3D12Resource* Output() const { return output_.Get(); }
    UINT64 Bytes() const { return bytes_; }
    static Ptr<ID3D12Resource> Buffer(ID3D12Device* device, UINT64 bytes,
        D3D12_HEAP_TYPE type, D3D12_RESOURCE_STATES state) {
        D3D12_HEAP_PROPERTIES heap{}; heap.Type = type;
        heap.CreationNodeMask = heap.VisibleNodeMask = 1;
        D3D12_RESOURCE_DESC desc{}; desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = bytes; desc.Height = desc.DepthOrArraySize = desc.MipLevels = 1;
        desc.SampleDesc.Count = 1; desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (type == D3D12_HEAP_TYPE_DEFAULT) desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        Ptr<ID3D12Resource> result;
        Check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state,
            nullptr, IID_PPV_ARGS(&result)), "pack buffer");
        return result;
    }
    static void Transition(ID3D12GraphicsCommandList* list, ID3D12Resource* resource,
        D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
        D3D12_RESOURCE_BARRIER b{}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition = {resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after};
        list->ResourceBarrier(1, &b);
    }
private:
    Ptr<ID3D12Device> device_;
    Ptr<ID3D12Resource> input_, output_, normalization_;
    Ptr<ID3D12RootSignature> root_;
    Ptr<ID3D12PipelineState> pipeline_;
    Ptr<ID3D12DescriptorHeap> descriptors_;
    UINT width_ = 0, height_ = 0;
    UINT64 bytes_ = 0;
    bool recorded_ = false;
};