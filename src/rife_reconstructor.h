#pragma once
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <vector>
#include <string>
#include <cmath>
#include <chrono>

// Render-thread only. CPU float32 NCHW flow/mask -> GPU warp of original A/B.
// Returns an owned texture view (never overwritten while retained by the timeline).
class RifeReconstructor {
    template<class T> using Ptr = Microsoft::WRL::ComPtr<T>;
public:
    struct Timing { double packMs=0, uploadMs=0, allocationMs=0, drawMs=0; };
    const Timing& LastTiming() const { return timing_; }
    double GpuMs() const { return gpuMs_; } // -1 until a valid asynchronous sample
    uint64_t GpuSamples() const { return gpuSamples_; }
    void PollGpuTiming(ID3D11DeviceContext* context) {
        if (!queryPending_) return;
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint = {};
        UINT64 begin=0, end=0;
        const HRESULT hr = context->GetData(disjoint_.Get(), &disjoint, sizeof(disjoint), D3D11_ASYNC_GETDATA_DONOTFLUSH);
        if (hr == S_FALSE) return;
        if (FAILED(hr) || disjoint.Disjoint || !disjoint.Frequency) { queryPending_=false; return; }
        const HRESULT a = context->GetData(begin_.Get(), &begin, sizeof(begin), D3D11_ASYNC_GETDATA_DONOTFLUSH);
        const HRESULT b = context->GetData(end_.Get(), &end, sizeof(end), D3D11_ASYNC_GETDATA_DONOTFLUSH);
        if (a == S_FALSE || b == S_FALSE) return;
        queryPending_=false;
        if (SUCCEEDED(a) && SUCCEEDED(b) && end >= begin) {
            gpuMs_ = double(end-begin)*1000.0/disjoint.Frequency;
            ++gpuSamples_;
        }
    }
    bool Initialize(ID3D11Device* device, std::string& error) {
        // Optional instrumentation: unsupported queries must not disable rendering.
        D3D11_QUERY_DESC q = {D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
        if (SUCCEEDED(device->CreateQuery(&q, &disjoint_))) {
            q.Query = D3D11_QUERY_TIMESTAMP;
            querySupported_ = SUCCEEDED(device->CreateQuery(&q, &begin_)) &&
                SUCCEEDED(device->CreateQuery(&q, &end_));
        }
        static const char code[] = R"(
Texture2D<float4> imageA : register(t0);
Texture2D<float4> imageB : register(t1);
Texture2D<float4> flow : register(t2);
Texture2D<float> mask : register(t3);
SamplerState linearClamp : register(s0);
struct V { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };
V VSMain(uint id : SV_VertexID) {
    V o; o.uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(o.uv * float2(2,-2) + float2(-1,1), 0, 1); return o;
}
float4 PSMain(V v) : SV_TARGET {
    uint w,h; flow.GetDimensions(w,h);
    // Half-pixel resize of flow/mask (align_corners=false), followed by border
    // bilinear warp. Displacements are in MODEL pixels, so UV offset is f/modelSize.
    // At matching resolution this equals PyTorch grid_sample align_corners=true.
    float4 f = flow.SampleLevel(linearClamp, v.uv, 0);
    float m = saturate(mask.SampleLevel(linearClamp, v.uv, 0));
    float3 a = imageA.SampleLevel(linearClamp, v.uv + f.xy / float2(w,h), 0).rgb;
    float3 b = imageB.SampleLevel(linearClamp, v.uv + f.zw / float2(w,h), 0).rgb;
    return float4(lerp(b,a,m),1);
})";
        Ptr<ID3DBlob> vs, ps, errors;
        HRESULT hr = D3DCompile(code, sizeof(code)-1, nullptr, nullptr, nullptr, "VSMain", "vs_5_0",
            D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &vs, &errors);
        if (SUCCEEDED(hr)) hr = D3DCompile(code, sizeof(code)-1, nullptr, nullptr, nullptr,
            "PSMain", "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &ps, &errors);
        if (FAILED(hr)) { error = errors ? std::string(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize()) : "warp compile failed"; return false; }
        if (FAILED(device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &vs_)) ||
            FAILED(device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &ps_))) return false;
        D3D11_SAMPLER_DESC s = {};
        s.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        s.AddressU = s.AddressV = s.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        s.MaxLOD = D3D11_FLOAT32_MAX;
        D3D11_RASTERIZER_DESC r = {};
        r.FillMode = D3D11_FILL_SOLID; r.CullMode = D3D11_CULL_NONE; r.DepthClipEnable = TRUE;
        return SUCCEEDED(device->CreateSamplerState(&s, &sampler_)) && SUCCEEDED(device->CreateRasterizerState(&r, &raster_));
    }

    bool Reconstruct(ID3D11Device* device, ID3D11DeviceContext* context,
        ID3D11ShaderResourceView* a, ID3D11ShaderResourceView* b,
        const std::vector<float>& fields, UINT mw, UINT mh,
        Ptr<ID3D11ShaderResourceView>& output) {
        output.Reset();
        timing_ = {};
        const auto start = Clock::now();
        const size_t plane = static_cast<size_t>(mw)*mh;
        if (!a || !b || !vs_ || !mw || !mh || fields.size() != plane*5) return false;
        for (float value : fields) if (!std::isfinite(value)) return false;
        Ptr<ID3D11Resource> ar, br;
        a->GetResource(&ar); b->GetResource(&br);
        Ptr<ID3D11Texture2D> at, bt;
        if (FAILED(ar.As(&at)) || FAILED(br.As(&bt))) return false;
        D3D11_TEXTURE2D_DESC ad={}, bd={}; at->GetDesc(&ad); bt->GetDesc(&bd);
        if (ad.Width != bd.Width || ad.Height != bd.Height) return false;
        std::vector<float> packed(plane*4);
        for (size_t i=0; i<plane; ++i) for (size_t c=0; c<4; ++c) packed[i*4+c] = fields[c*plane+i];
        const auto packedAt = Clock::now();
        timing_.packMs = Ms(start, packedAt);
        D3D11_TEXTURE2D_DESC d={}; d.Width=mw; d.Height=mh;
        d.MipLevels=d.ArraySize=d.SampleDesc.Count=1;
        d.Usage=D3D11_USAGE_IMMUTABLE; d.BindFlags=D3D11_BIND_SHADER_RESOURCE;
        d.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;
        D3D11_SUBRESOURCE_DATA data={packed.data(), mw*16, 0};
        Ptr<ID3D11Texture2D> ft,mt,target;
        Ptr<ID3D11ShaderResourceView> fv,mv;
        if (FAILED(device->CreateTexture2D(&d,&data,&ft)) || FAILED(device->CreateShaderResourceView(ft.Get(),nullptr,&fv))) return false;
        d.Format=DXGI_FORMAT_R32_FLOAT; data.pSysMem=fields.data()+plane*4; data.SysMemPitch=mw*4;
        if (FAILED(device->CreateTexture2D(&d,&data,&mt)) || FAILED(device->CreateShaderResourceView(mt.Get(),nullptr,&mv))) return false;
        const auto uploadedAt = Clock::now();
        timing_.uploadMs = Ms(packedAt, uploadedAt);
        d.Width=ad.Width; d.Height=ad.Height; d.Format=DXGI_FORMAT_B8G8R8A8_UNORM;
        d.Usage=D3D11_USAGE_DEFAULT; d.BindFlags |= D3D11_BIND_RENDER_TARGET;
        Ptr<ID3D11RenderTargetView> rtv;
        if (FAILED(device->CreateTexture2D(&d,nullptr,&target)) || FAILED(device->CreateRenderTargetView(target.Get(),nullptr,&rtv)) ||
            FAILED(device->CreateShaderResourceView(target.Get(),nullptr,&output))) return false;
        const auto allocatedAt = Clock::now();
        timing_.allocationMs = Ms(uploadedAt, allocatedAt);
        PollGpuTiming(context);
        const bool measure = querySupported_ && !queryPending_;
        if (measure) { context->Begin(disjoint_.Get()); context->End(begin_.Get()); }
        ID3D11ShaderResourceView* nulls[4]={};
        context->PSSetShaderResources(0,4,nulls);
        context->OMSetRenderTargets(1,rtv.GetAddressOf(),nullptr);
        context->OMSetBlendState(nullptr,nullptr,0xffffffff); context->OMSetDepthStencilState(nullptr,0);
        context->RSSetState(raster_.Get());
        D3D11_VIEWPORT vp={0,0,static_cast<float>(d.Width),static_cast<float>(d.Height),0,1};
        context->RSSetViewports(1,&vp);
        context->IASetInputLayout(nullptr); context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->VSSetShader(vs_.Get(),nullptr,0); context->GSSetShader(nullptr,nullptr,0); context->PSSetShader(ps_.Get(),nullptr,0);
        ID3D11ShaderResourceView* views[]={a,b,fv.Get(),mv.Get()};
        context->PSSetShaderResources(0,4,views); context->PSSetSamplers(0,1,sampler_.GetAddressOf()); context->Draw(3,0);
        context->PSSetShaderResources(0,4,nulls); context->OMSetRenderTargets(0,nullptr,nullptr); context->RSSetState(nullptr);
        if (measure) { context->End(end_.Get()); context->End(disjoint_.Get()); queryPending_=true; }
        timing_.drawMs = Ms(allocatedAt, Clock::now());
        return true;
    }
private:
    using Clock = std::chrono::steady_clock;
    static double Ms(Clock::time_point a, Clock::time_point b) {
        return std::chrono::duration<double, std::milli>(b-a).count();
    }
    Timing timing_;
    Ptr<ID3D11Query> disjoint_, begin_, end_;
    bool querySupported_ = false, queryPending_ = false;
    double gpuMs_ = -1;
    uint64_t gpuSamples_ = 0;
    Ptr<ID3D11VertexShader> vs_;
    Ptr<ID3D11PixelShader> ps_;
    Ptr<ID3D11SamplerState> sampler_;
    Ptr<ID3D11RasterizerState> raster_;
};