#pragma once
#include "plain_texture.h"
#include "texture_handoff.h"
#include "artifacts.h"
#include "interop/baseline/rife_reconstructor.h"
#include <d3d12sdklayers.h>
#include <dxgi1_4.h>
#include <algorithm>
#include <iostream>
#include <type_traits>

using Microsoft::WRL::ComPtr;
static_assert(!std::is_copy_constructible_v<TextureHandoff>);
static_assert(!std::is_move_constructible_v<TextureHandoff>);
static void Check(HRESULT hr, const char* text) { GpuOrtInputs::Check(hr, text); }
static void Require(bool condition, const char* text) { if (!condition) throw std::runtime_error(text); }
#include "adapter_selection.h"
static unsigned rejections = 0;
template<class F> static void Reject(F&& f) {
    bool caught = false; try { f(); } catch (const std::exception&) { caught = true; }
    Require(caught, "invalid reconstruction operation accepted"); ++rejections;
}
static HRESULT FailSignal(ID3D12CommandQueue*, ID3D12Fence*, UINT64) { return E_FAIL; }

struct Commands {
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    explicit Commands(ID3D12Device* device) {
        Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)), "test allocator");
        Check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
            IID_PPV_ARGS(&list)), "test list");
    }
    void Execute(ID3D12CommandQueue* queue) {
        Check(list->Close(), "test list close"); ID3D12CommandList* lists[]{list.Get()};
        queue->ExecuteCommandLists(1, lists);
    }
};
static void Drain(GpuProviderContext& context) {
    ComPtr<ID3D12Fence> fence;
    Check(context.Device()->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "test fence");
    Check(context.Queue()->Signal(fence.Get(),1), "test signal");
    const auto start = std::chrono::steady_clock::now();
    while (fence->GetCompletedValue() < 1) {
        Check(context.Device()->GetDeviceRemovedReason(), "test device removed");
        if (std::chrono::steady_clock::now()-start > std::chrono::seconds(30)) std::terminate();
        Sleep(1);
    }
    Check(context.Device()->GetDeviceRemovedReason(), "test drain device");
}
static ComPtr<ID3D12Resource> Texture(ID3D12Device* device, UINT w, UINT h,
    DXGI_FORMAT format = DXGI_FORMAT_B8G8R8A8_UNORM) {
    D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    heap.CreationNodeMask = heap.VisibleNodeMask = 1;
    D3D12_RESOURCE_DESC d{}; d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width=w; d.Height=h; d.DepthOrArraySize=d.MipLevels=1; d.SampleDesc.Count=1; d.Format=format;
    ComPtr<ID3D12Resource> result;
    Check(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&d,D3D12_RESOURCE_STATE_COMMON,
        nullptr,IID_PPV_ARGS(&result)), "test texture"); return result;
}
static std::vector<unsigned char> Pixels(UINT w, UINT h, unsigned image, bool hud) {
    std::vector<unsigned char> result(static_cast<size_t>(w)*h*4);
    for (UINT y=0; y<h; ++y) for(UINT x=0; x<w; ++x) {
        const size_t offset=(static_cast<size_t>(y)*w+x)*4;
        for(UINT c=0;c<3;++c) {
            const UINT v=hud ? ((x%31==0 || y%23==0 || (x%31<9 && y%23==7)) ? 255u : (c+1)*13u)
                : (x*173u/w+y*61u/h+c*29u+image*47u)%256u;
            result[offset+c]=static_cast<unsigned char>(v);
        }
        result[offset+3]=255;
    }
    return result;
}
static std::vector<float> Fields(unsigned mode) {
    constexpr size_t plane=576*320;
    std::vector<float> result(plane*5);
    for(UINT y=0;y<320;++y) for(UINT x=0;x<576;++x) {
        float v[5]{0,0,0,0,1};
        if(mode==1) v[4]=0;
        if(mode==2) v[4]=0.5f;
        if(mode==3 || mode==10) { v[0]=0.375f; v[1]=-0.25f; v[2]=-0.625f; v[3]=0.125f; v[4]=0.375f; }
        if(mode==4) {
            v[0]=static_cast<float>(static_cast<int>(x%17)-8)*0.125f;
            v[1]=static_cast<float>(static_cast<int>(y%13)-6)*0.0625f;
            v[2]=-v[0]*0.5f; v[3]=-v[1]*2;
            v[4]=static_cast<float>((x+3*y)%257)/256.f;
        }
        if(mode==5) { v[0]=2048; v[1]=-2048; v[2]=-2048; v[3]=2048; v[4]=0.5f; }
        if(mode==6) v[4]=-2;
        if(mode==7) v[4]=3;
        for(size_t c=0;c<5;++c) result[c*plane+static_cast<size_t>(y)*576+x]=v[c];
    }
    return result;
}

struct Writer {
    Commands commands;
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> pipeline;
    explicit Writer(ID3D12Device* device) : commands(device) {
        D3D12_ROOT_PARAMETER params[2]{}; params[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_UAV;
        params[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS; params[1].Constants.Num32BitValues=1;
        D3D12_ROOT_SIGNATURE_DESC desc{}; desc.NumParameters=2; desc.pParameters=params;
        ComPtr<ID3DBlob> signature, errors, shader;
        Check(D3D12SerializeRootSignature(&desc,D3D_ROOT_SIGNATURE_VERSION_1,&signature,&errors),"writer root serialize");
        Check(device->CreateRootSignature(0,signature->GetBufferPointer(),signature->GetBufferSize(),IID_PPV_ARGS(&root)),"writer root");
        static const char code[]=R"(
RWByteAddressBuffer fields : register(u0);
cbuffer Mode : register(b0) { uint mode; };
[numthreads(16,16,1)] void main(uint3 p : SV_DispatchThreadID) {
 if(p.x>=576 || p.y>=320) return;
 float4 f=0; float m=1;
 if(mode==1) m=0; if(mode==2) m=0.5f;
 if(mode==3 || mode==10) { f=float4(0.375f,-0.25f,-0.625f,0.125f); m=0.375f; }
 if(mode==4) {
   f.x=(int(p.x%17)-8)*0.125f; f.y=(int(p.y%13)-6)*0.0625f;
   f.z=-f.x*0.5f; f.w=-f.y*2; m=float((p.x+3*p.y)%257)/256.f;
 }
 if(mode==5) { f=float4(2048,-2048,-2048,2048); m=0.5f; }
 if(mode==6) m=-2; if(mode==7) m=3;
 uint i=p.y*576+p.x;
 fields.Store(4*i,asuint(f.x)); fields.Store(4*(184320+i),asuint(f.y));
 fields.Store(4*(368640+i),asuint(f.z)); fields.Store(4*(552960+i),asuint(f.w));
 fields.Store(4*(737280+i),asuint(m));
})";
        Check(D3DCompile(code,sizeof(code)-1,nullptr,nullptr,nullptr,"main","cs_5_0",
            D3DCOMPILE_OPTIMIZATION_LEVEL3|D3DCOMPILE_WARNINGS_ARE_ERRORS,0,&shader,&errors),"writer shader");
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd{}; pd.pRootSignature=root.Get();
        pd.CS={shader->GetBufferPointer(),shader->GetBufferSize()};
        Check(device->CreateComputePipelineState(&pd,IID_PPV_ARGS(&pipeline)),"writer pipeline");
    }
    void Execute(ID3D12Resource* flow, ID3D12CommandQueue* queue, unsigned mode) {
        auto* list=commands.list.Get(); list->SetPipelineState(pipeline.Get()); list->SetComputeRootSignature(root.Get());
        list->SetComputeRootUnorderedAccessView(0,flow->GetGPUVirtualAddress()); list->SetComputeRoot32BitConstant(1,mode,0);
        list->Dispatch(36,20,1); commands.Execute(queue);
        // Consumer's UAV->SRV transition orders the writer before field reads.
    }
};
struct Oracle {
    Commands commands;
    ComPtr<ID3D12Resource> buffer;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT w,h;
    Oracle(ID3D12Device* device, ID3D12Resource* texture) : commands(device) {
        const auto d=texture->GetDesc(); w=static_cast<UINT>(d.Width); h=d.Height; UINT64 bytes=0;
        device->GetCopyableFootprints(&d,0,1,0,&footprint,nullptr,nullptr,&bytes);
        buffer=GpuInputPack::Buffer(device,bytes,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
        GpuInputPack::Transition(commands.list.Get(),texture,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION dst{}; dst.pResource=buffer.Get(); dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint=footprint;
        D3D12_TEXTURE_COPY_LOCATION src{}; src.pResource=texture; src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        commands.list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
        GpuInputPack::Transition(commands.list.Get(),texture,D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COMMON);
    }
    std::vector<unsigned char> Read() {
        void* mapped=nullptr; D3D12_RANGE read{0,static_cast<SIZE_T>(buffer->GetDesc().Width)}, empty{0,0};
        Check(buffer->Map(0,&read,&mapped),"oracle map");
        std::vector<unsigned char> result(static_cast<size_t>(w)*h*4);
        for(UINT y=0;y<h;++y) std::memcpy(result.data()+static_cast<size_t>(y)*w*4,
            static_cast<unsigned char*>(mapped)+footprint.Offset+static_cast<size_t>(y)*footprint.Footprint.RowPitch,
            static_cast<size_t>(w)*4);
        buffer->Unmap(0,&empty); return result;
    }
};
static void Upload(GpuProviderContext& context, ID3D12Resource* texture, const std::vector<unsigned char>& pixels) {
    const auto d=texture->GetDesc(); D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{}; UINT64 bytes=0;
    context.Device()->GetCopyableFootprints(&d,0,1,0,&fp,nullptr,nullptr,&bytes);
    auto upload=GpuInputPack::Buffer(context.Device(),bytes,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
    void* mapped=nullptr; D3D12_RANGE empty{0,0}; Check(upload->Map(0,&empty,&mapped),"fixture upload map");
    for(UINT y=0;y<d.Height;++y) std::memcpy(static_cast<unsigned char*>(mapped)+fp.Offset+static_cast<size_t>(y)*fp.Footprint.RowPitch,
        pixels.data()+static_cast<size_t>(y)*static_cast<size_t>(d.Width)*4,static_cast<size_t>(d.Width)*4);
    upload->Unmap(0,nullptr);
    Commands commands(context.Device());
    GpuInputPack::Transition(commands.list.Get(),texture,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION dst{}; dst.pResource=texture; dst.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION src{}; src.pResource=upload.Get(); src.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; src.PlacedFootprint=fp;
    commands.list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
    GpuInputPack::Transition(commands.list.Get(),texture,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COMMON);
    commands.Execute(context.Queue()); Drain(context);
}
static std::vector<unsigned char> Reference(ID3D11Device* device, ID3D11DeviceContext* context,
    RifeReconstructor& recon, UINT w, UINT h, const std::vector<unsigned char>& a,
    const std::vector<unsigned char>& b, unsigned mode) {
    auto image=[&](const std::vector<unsigned char>& pixels) {
        D3D11_TEXTURE2D_DESC d{}; d.Width=w; d.Height=h; d.MipLevels=d.ArraySize=d.SampleDesc.Count=1;
        d.Format=DXGI_FORMAT_B8G8R8A8_UNORM; d.Usage=D3D11_USAGE_IMMUTABLE; d.BindFlags=D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA s{pixels.data(),w*4,0}; ComPtr<ID3D11Texture2D> texture; ComPtr<ID3D11ShaderResourceView> view;
        Check(device->CreateTexture2D(&d,&s,&texture),"reference input");
        Check(device->CreateShaderResourceView(texture.Get(),nullptr,&view),"reference view"); return view;
    };
    auto av=image(a),bv=image(b); ComPtr<ID3D11ShaderResourceView> output;
    Require(recon.Reconstruct(device,context,av.Get(),bv.Get(),Fields(mode),576,320,output),"reference reconstruction");
    ComPtr<ID3D11Resource> resource; output->GetResource(&resource); ComPtr<ID3D11Texture2D> texture;
    Check(resource.As(&texture),"reference output"); D3D11_TEXTURE2D_DESC d{}; texture->GetDesc(&d);
    d.Usage=D3D11_USAGE_STAGING; d.BindFlags=0; d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging; Check(device->CreateTexture2D(&d,nullptr,&staging),"reference staging");
    context->CopyResource(staging.Get(),texture.Get()); D3D11_MAPPED_SUBRESOURCE map{};
    Check(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&map),"reference map");
    std::vector<unsigned char> pixels(static_cast<size_t>(w)*h*4);
    for(UINT y=0;y<h;++y) std::memcpy(pixels.data()+static_cast<size_t>(y)*w*4,
        static_cast<unsigned char*>(map.pData)+static_cast<size_t>(y)*map.RowPitch,static_cast<size_t>(w)*4);
    context->Unmap(staging.Get(),0); return pixels;
}
// Readback is an oracle ONLY, not part of the shared-texture handoff.
struct ConsumerOracle {
    ComPtr<ID3D11Texture2D> staging;
    UINT w, h;
    RifePreprocessor draw;
    ConsumerOracle(ID3D11Device* device, UINT width, UINT height) : w(width), h(height) {
        D3D11_TEXTURE2D_DESC d{}; d.Width=w; d.Height=h;
        d.MipLevels=d.ArraySize=d.SampleDesc.Count=1;
        d.Format=DXGI_FORMAT_B8G8R8A8_UNORM; d.Usage=D3D11_USAGE_STAGING;
        d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        Check(device->CreateTexture2D(&d,nullptr,&staging),"consumer oracle staging");
        std::string error; Require(draw.Initialize(device,w,h,error),error.c_str());
    }
    void Execute(ID3D11DeviceContext* immediate, ID3D11ShaderResourceView* view) {
        Require(draw.Resize(immediate,view),"shared D3D11 draw");
        ComPtr<ID3D11Resource> resource; draw.OutputView()->GetResource(&resource);
        immediate->CopyResource(staging.Get(),resource.Get());
    }
    std::vector<unsigned char> Read(ID3D11DeviceContext* immediate) {
        D3D11_MAPPED_SUBRESOURCE map{};
        Check(immediate->Map(staging.Get(),0,D3D11_MAP_READ,0,&map),"consumer oracle map");
        std::vector<unsigned char> result(static_cast<size_t>(w)*h*4);
        for(UINT y=0;y<h;++y) std::memcpy(result.data()+static_cast<size_t>(y)*w*4,
            static_cast<unsigned char*>(map.pData)+static_cast<size_t>(y)*map.RowPitch,static_cast<size_t>(w)*4);
        immediate->Unmap(staging.Get(),0); return result;
    }
};
