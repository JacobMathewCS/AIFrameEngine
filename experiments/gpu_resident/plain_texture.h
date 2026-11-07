#pragma once
#include "interop/gpu_flow_output.h"

// Scratch-only, single-use GPU consumer. No Map, CPU fields, or flow texture upload.
// Sources: same-device BGRA8 UNORM, ready in COMMON. Flow: finite FP32 NCHW
// [1,5,320,576] in UAV state, produced earlier on the exact context queue.
// Submit restores source COMMON / flow UAV; output is owned BGRA8 in COMMON.
// Caller MUST retain this object through completion (e.g. GpuResidentRun hooks
// or GpuFlowOutput dependencies). Output borrowing alone does not retain the job.
class PlainTexture {
    template<class T> using Ptr = Microsoft::WRL::ComPtr<T>;
    static void Check(HRESULT hr, const char* message) { GpuOrtInputs::Check(hr, message); }
    void SameDevice(ID3D12DeviceChild* resource) const {
        Ptr<ID3D12Device> owner;
        Check(resource->GetDevice(IID_PPV_ARGS(&owner)), "reconstruction resource device");
        if (!GpuOrtInputs::SameObject(owner.Get(), context_->Device()))
            throw std::invalid_argument("reconstruction device mismatch");
    }
    D3D12_RESOURCE_DESC Source(ID3D12Resource* resource) const {
        if (!resource) throw std::invalid_argument("null reconstruction source");
        SameDevice(resource);
        const auto d = resource->GetDesc();
        if (d.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || d.Format != DXGI_FORMAT_B8G8R8A8_UNORM ||
            !d.Width || !d.Height || d.Width > 16384 || d.Height > 16384 || d.DepthOrArraySize != 1 ||
            d.MipLevels != 1 || d.SampleDesc.Count != 1 || (d.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE))
            throw std::invalid_argument("reconstruction requires single BGRA8 source");
        return d;
    }
public:
    PlainTexture(std::shared_ptr<GpuProviderContext> context, ID3D12Resource* a, ID3D12Resource* b,
        bool sharedOutput = false)
        : context_(std::move(context)), a_(a), b_(b) {
        if (!context_) throw std::invalid_argument("null reconstruction context");
        const auto ad = Source(a), bd = Source(b);
        if (ad.Width != bd.Width || ad.Height != bd.Height)
            throw std::invalid_argument("reconstruction source dimensions differ");
        width_ = static_cast<UINT>(ad.Width); height_ = ad.Height;
        auto* device = context_->Device();
        D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        heap.CreationNodeMask = heap.VisibleNodeMask = 1;
        auto target = ad; target.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        if (sharedOutput) target.Flags |= D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;
        target.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN; target.Alignment = 0;
        Check(device->CreateCommittedResource(&heap, sharedOutput ? D3D12_HEAP_FLAG_SHARED : D3D12_HEAP_FLAG_NONE, &target,
            D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&output_)), "reconstruction output");
        D3D12_DESCRIPTOR_HEAP_DESC hd{}; hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.NumDescriptors = 3; hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        Check(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&views_)), "reconstruction views");
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{}; srv.Format = ad.Format;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D; srv.Texture2D.MipLevels = 1;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        auto cpu = views_->GetCPUDescriptorHandleForHeapStart();
        device->CreateShaderResourceView(a, &srv, cpu);
        cpu.ptr += device->GetDescriptorHandleIncrementSize(hd.Type);
        device->CreateShaderResourceView(b, &srv, cpu);
        D3D12_RESOURCE_DESC td{}; td.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        td.Width=576; td.Height=320; td.DepthOrArraySize=5; td.MipLevels=1;
        td.Format=DXGI_FORMAT_R32_FLOAT; td.SampleDesc.Count=1;
        Check(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&td,D3D12_RESOURCE_STATE_COMMON,
            nullptr,IID_PPV_ARGS(&fieldTexture_)),"field texture array");
        device->GetCopyableFootprints(&td,0,5,0,footprints_.data(),nullptr,nullptr,&textureBytes_);
        if(textureBytes_!=GpuFlowOutput::Bytes) throw std::runtime_error("unexpected field texture layout");
        for(UINT c=0;c<5;++c) if(footprints_[c].Offset!=static_cast<UINT64>(c)*576*320*4 ||
            footprints_[c].Footprint.RowPitch!=576*4) throw std::runtime_error("nonpacked field texture layout");
        cpu.ptr+=device->GetDescriptorHandleIncrementSize(hd.Type);
        D3D12_SHADER_RESOURCE_VIEW_DESC ts{}; ts.Format=td.Format; ts.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
        ts.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        ts.Texture2DArray.MipLevels=1; ts.Texture2DArray.ArraySize=5;
        device->CreateShaderResourceView(fieldTexture_.Get(),&ts,cpu);
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; hd.NumDescriptors = 1; hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        Check(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtv_)), "reconstruction RTV heap");
        device->CreateRenderTargetView(output_.Get(), nullptr, rtv_->GetCPUDescriptorHandleForHeapStart());
        D3D12_DESCRIPTOR_RANGE range{}; range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        range.NumDescriptors = 3; range.BaseShaderRegister = 0;
        D3D12_ROOT_PARAMETER params[2]{};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[0].DescriptorTable = {1, &range}; params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        params[1].Descriptor.ShaderRegister = 3; params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_STATIC_SAMPLER_DESC sampler{}; sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.MaxLOD = D3D12_FLOAT32_MAX; sampler.MaxAnisotropy = 1;
        sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS; sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_ROOT_SIGNATURE_DESC rd{}; rd.NumParameters = 2; rd.pParameters = params;
        rd.NumStaticSamplers = 1; rd.pStaticSamplers = &sampler;
        Ptr<ID3DBlob> signature, errors;
        Check(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &errors), "reconstruction root serialize");
        Check(device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(),
            IID_PPV_ARGS(&root_)), "reconstruction root");
        static const char shader[] = R"(
Texture2D<float4> imageA : register(t0);
Texture2D<float4> imageB : register(t1);
Texture2DArray<float> fields : register(t2);
SamplerState linearClamp : register(s0);
struct V { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };
V VSMain(uint id : SV_VertexID) {
    V o; o.uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(o.uv * float2(2,-2) + float2(-1,1), 0, 1); return o;
}
float4 PSMain(V v) : SV_TARGET {
    float4 f=float4(fields.SampleLevel(linearClamp,float3(v.uv,0),0),
                    fields.SampleLevel(linearClamp,float3(v.uv,1),0),
                    fields.SampleLevel(linearClamp,float3(v.uv,2),0),
                    fields.SampleLevel(linearClamp,float3(v.uv,3),0));
    float m=saturate(fields.SampleLevel(linearClamp,float3(v.uv,4),0));
    float3 a = imageA.SampleLevel(linearClamp, v.uv + f.xy/float2(576,320),0).rgb;
    float3 b = imageB.SampleLevel(linearClamp, v.uv + f.zw/float2(576,320),0).rgb;
    return float4(lerp(b,a,m),1);
})";
        auto compile = [&](const char* entry, const char* profile) {
            Ptr<ID3DBlob> code;
            const HRESULT hr = D3DCompile(shader, sizeof(shader)-1, nullptr, nullptr, nullptr, entry, profile,
                D3DCOMPILE_OPTIMIZATION_LEVEL3 | D3DCOMPILE_WARNINGS_ARE_ERRORS, 0, &code, &errors);
            if (FAILED(hr) && errors) throw std::runtime_error(std::string(
                static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize()));
            Check(hr, "reconstruction shader"); return code;
        };
        auto vs = compile("VSMain", "vs_5_0"), ps = compile("PSMain", "ps_5_0");
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{}; pd.pRootSignature = root_.Get();
        pd.VS = {vs->GetBufferPointer(), vs->GetBufferSize()}; pd.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
        auto& blend = pd.BlendState.RenderTarget[0]; blend.SrcBlend = blend.SrcBlendAlpha = D3D12_BLEND_ONE;
        blend.DestBlend = blend.DestBlendAlpha = D3D12_BLEND_ZERO;
        blend.BlendOp = blend.BlendOpAlpha = D3D12_BLEND_OP_ADD; blend.LogicOp = D3D12_LOGIC_OP_NOOP;
        blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        pd.SampleMask = UINT_MAX; pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE; pd.RasterizerState.DepthClipEnable = TRUE;
        pd.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pd.NumRenderTargets = 1; pd.RTVFormats[0] = DXGI_FORMAT_B8G8R8A8_UNORM; pd.SampleDesc.Count = 1;
        Check(device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pipeline_)), "reconstruction pipeline");
        Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator_)), "reconstruction allocator");
        Check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator_.Get(), pipeline_.Get(),
            IID_PPV_ARGS(&list_)), "reconstruction list");
    }
    PlainTexture(const PlainTexture&) = delete;
    PlainTexture& operator=(const PlainTexture&) = delete;
    PlainTexture(PlainTexture&&) = delete;
    PlainTexture& operator=(PlainTexture&&) = delete;
    void Submit(ID3D12Resource* flow, ID3D12CommandQueue* queue) {
        if (submitted_) throw std::logic_error("reconstruction already submitted");
        if (!flow || !queue || !GpuOrtInputs::SameObject(queue, context_->Queue()))
            throw std::invalid_argument("reconstruction null flow or wrong queue");
        SameDevice(flow);
        const auto d = flow->GetDesc();
        D3D12_HEAP_PROPERTIES heap{}; D3D12_HEAP_FLAGS flags{};
        Check(flow->GetHeapProperties(&heap, &flags), "flow heap");
        if (d.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER || d.Width != GpuFlowOutput::Bytes ||
            d.Format != DXGI_FORMAT_UNKNOWN || heap.Type != D3D12_HEAP_TYPE_DEFAULT ||
            !(d.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) ||
            (d.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE))
            throw std::invalid_argument("reconstruction requires exact resident FP32 flow buffer");
        flow_ = flow; submitted_ = true;
        auto transition = [&](ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
            GpuInputPack::Transition(list_.Get(), resource, before, after);
        };
        transition(flow,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);
        transition(fieldTexture_.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_DEST);
        for(UINT c=0;c<5;++c) {
            D3D12_TEXTURE_COPY_LOCATION src{}; src.pResource=flow;
            src.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; src.PlacedFootprint=footprints_[c];
            D3D12_TEXTURE_COPY_LOCATION dst{}; dst.pResource=fieldTexture_.Get();
            dst.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; dst.SubresourceIndex=c;
            list_->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
        }
        transition(flow,D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        transition(fieldTexture_.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        transition(a_.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        if (!GpuOrtInputs::SameObject(a_.Get(), b_.Get()))
            transition(b_.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        transition(output_.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET);
        list_->SetGraphicsRootSignature(root_.Get()); ID3D12DescriptorHeap* heaps[]{views_.Get()};
        list_->SetDescriptorHeaps(1, heaps);
        list_->SetGraphicsRootDescriptorTable(0, views_->GetGPUDescriptorHandleForHeapStart());
        list_->SetGraphicsRootShaderResourceView(1, flow->GetGPUVirtualAddress());
        D3D12_VIEWPORT viewport{0,0,static_cast<float>(width_),static_cast<float>(height_),0,1};
        D3D12_RECT scissor{0,0,static_cast<LONG>(width_),static_cast<LONG>(height_)};
        list_->RSSetViewports(1, &viewport); list_->RSSetScissorRects(1, &scissor);
        const auto rtv = rtv_->GetCPUDescriptorHandleForHeapStart(); list_->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        list_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST); list_->DrawInstanced(3,1,0,0);
        transition(output_.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COMMON);
        transition(a_.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
        if (!GpuOrtInputs::SameObject(a_.Get(), b_.Get()))
            transition(b_.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
        transition(flow, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        transition(fieldTexture_.Get(),D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COMMON);
        Check(list_->Close(), "reconstruction close");
        ID3D12CommandList* lists[]{list_.Get()}; queue->ExecuteCommandLists(1, lists);
    }
    ID3D12Resource* Output() const { return output_.Get(); }
    ID3D12Resource* FieldTexture() const { return fieldTexture_.Get(); }
    const std::array<D3D12_PLACED_SUBRESOURCE_FOOTPRINT,5>& Footprints() const { return footprints_; }
private:
    std::shared_ptr<GpuProviderContext> context_;
    Ptr<ID3D12Resource> a_, b_, flow_, output_;
    Ptr<ID3D12DescriptorHeap> views_, rtv_;
    Ptr<ID3D12RootSignature> root_;
    Ptr<ID3D12PipelineState> pipeline_;
    Ptr<ID3D12CommandAllocator> allocator_;
    Ptr<ID3D12GraphicsCommandList> list_;
    UINT width_ = 0, height_ = 0;
    bool submitted_ = false;
    Ptr<ID3D12Resource> fieldTexture_;
    std::array<D3D12_PLACED_SUBRESOURCE_FOOTPRINT,5> footprints_{};
    UINT64 textureBytes_=0;
};