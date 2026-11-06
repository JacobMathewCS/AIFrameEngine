#pragma once
#include "interop/gpu_flow_output.h"

// One-shot, exact IEEE exponent test, not floating-point fast-math isfinite.
// Retain this object with the outer submission owner through queue retirement.
// Only a four-byte decision crosses to CPU. No field/pixel readback here.
class FiniteFields {
    template<class T> using Ptr = Microsoft::WRL::ComPtr<T>;
    static void Check(HRESULT hr, const char* text) { GpuOrtInputs::Check(hr, text); }
public:
    explicit FiniteFields(std::shared_ptr<GpuProviderContext> context) : context_(std::move(context)) {
        if (!context_) throw std::invalid_argument("finite scan requires context");
        auto* device = context_->Device();
        status_ = GpuInputPack::Buffer(device, 4, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        readback_ = GpuInputPack::Buffer(device, 4, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_ROOT_PARAMETER params[2]{};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        D3D12_ROOT_SIGNATURE_DESC rd{}; rd.NumParameters = 2; rd.pParameters = params;
        Ptr<ID3DBlob> signature, errors, shader;
        Check(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &errors), "finite root serialize");
        Check(device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(), IID_PPV_ARGS(&root_)), "finite root");
        const char code[] = R"(
ByteAddressBuffer fields : register(t0);
RWByteAddressBuffer status : register(u0);
[numthreads(1,1,1)] void reset(uint3 p : SV_DispatchThreadID) { status.Store(0,0); }
[numthreads(256,1,1)] void scan(uint3 p : SV_DispatchThreadID) {
    if(p.x < 921600 && (fields.Load(p.x*4) & 0x7f800000u) == 0x7f800000u) {
        uint old; status.InterlockedOr(0,1,old);
    }
})";
        for (unsigned i = 0; i < 2; ++i) {
            Check(D3DCompile(code, sizeof(code)-1, nullptr, nullptr, nullptr, i ? "scan" : "reset", "cs_5_0",
                D3DCOMPILE_OPTIMIZATION_LEVEL3 | D3DCOMPILE_WARNINGS_ARE_ERRORS, 0, &shader, &errors), "finite shader");
            D3D12_COMPUTE_PIPELINE_STATE_DESC pd{}; pd.pRootSignature = root_.Get();
            pd.CS = {shader->GetBufferPointer(), shader->GetBufferSize()};
            Check(device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&pipelines_[i])), "finite pipeline");
        }
        Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator_)), "finite allocator");
        Check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator_.Get(), nullptr, IID_PPV_ARGS(&list_)), "finite list");
        Check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_)), "finite fence");
    }
    FiniteFields(const FiniteFields&) = delete;
    FiniteFields& operator=(const FiniteFields&) = delete;
    bool CheckFinite(ID3D12Resource* fields, ID3D12CommandQueue* queue, DWORD timeoutMs) {
        if (submitted_) throw std::logic_error("finite scan already submitted");
        if (!fields || !queue || !GpuOrtInputs::SameObject(queue, context_->Queue()) || timeoutMs == INFINITE)
            throw std::invalid_argument("finite scan queue/resource/timeout");
        Ptr<ID3D12Device> device; Check(fields->GetDevice(IID_PPV_ARGS(&device)), "finite field device");
        const auto d = fields->GetDesc();
        if (!GpuOrtInputs::SameObject(device.Get(), context_->Device()) || d.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER ||
            d.Width != GpuFlowOutput::Bytes) throw std::invalid_argument("finite field contract");
        GpuInputPack::Transition(list_.Get(), fields, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        list_->SetComputeRootSignature(root_.Get());
        list_->SetComputeRootShaderResourceView(0, fields->GetGPUVirtualAddress());
        list_->SetComputeRootUnorderedAccessView(1, status_->GetGPUVirtualAddress());
        list_->SetPipelineState(pipelines_[0].Get()); list_->Dispatch(1,1,1);
        D3D12_RESOURCE_BARRIER barrier{}; barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV; barrier.UAV.pResource = status_.Get();
        list_->ResourceBarrier(1, &barrier);
        list_->SetPipelineState(pipelines_[1].Get()); list_->Dispatch(3600,1,1);
        GpuInputPack::Transition(list_.Get(), status_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        list_->CopyBufferRegion(readback_.Get(), 0, status_.Get(), 0, 4);
        GpuInputPack::Transition(list_.Get(), fields, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Check(list_->Close(), "finite close");
        submitted_ = true;
        ID3D12CommandList* lists[]{list_.Get()}; queue->ExecuteCommandLists(1, lists);
        Check(queue->Signal(fence_.Get(), 1), "finite signal");
        const auto start = std::chrono::steady_clock::now();
        while (fence_->GetCompletedValue() < 1) {
            Check(context_->Device()->GetDeviceRemovedReason(), "finite device removed");
            if (std::chrono::steady_clock::now() - start >= std::chrono::milliseconds(timeoutMs))
                throw std::runtime_error("finite status timeout; retain until outer retirement");
            Sleep(1);
        }
        Check(context_->Device()->GetDeviceRemovedReason(), "finite completed device");
        void* mapped = nullptr; D3D12_RANGE read{0,4}, empty{0,0};
        Check(readback_->Map(0, &read, &mapped), "finite status map");
        UINT status = 0; std::memcpy(&status, mapped, 4); readback_->Unmap(0, &empty);
        if (status > 1) throw std::runtime_error("invalid finite status");
        return status == 0;
    }
private:
    std::shared_ptr<GpuProviderContext> context_;
    Ptr<ID3D12Resource> status_, readback_;
    Ptr<ID3D12RootSignature> root_;
    std::array<Ptr<ID3D12PipelineState>,2> pipelines_;
    Ptr<ID3D12CommandAllocator> allocator_;
    Ptr<ID3D12GraphicsCommandList> list_;
    Ptr<ID3D12Fence> fence_;
    bool submitted_ = false;
};