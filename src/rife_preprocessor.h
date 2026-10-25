#pragma once
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

// Render-thread only. Input SRVs must reference immutable, owned capture images.
// Changes graphics state; the compositor rebinds its display pipeline afterwards.
// Readback is intentionally synchronous for now, but only at model resolution.
class RifePreprocessor {
public:
    bool Initialize(ID3D11Device* device, UINT width, UINT height, std::string& error) {
        if (!device || !width || !height) { error = "invalid preprocessing dimensions/device"; return false; }
        width_ = width;
        height_ = height;
        static const char shader[] = R"(
            Texture2D image : register(t0);
            SamplerState linearClamp : register(s0);
            struct V { float4 position : SV_POSITION; float2 uv : TEXCOORD0; };
            V VSMain(uint id : SV_VertexID) {
                V v;
                v.uv = float2((id << 1) & 2, id & 2);
                v.position = float4(v.uv * float2(2, -2) + float2(-1, 1), 0, 1);
                return v;
            }
            float4 PSMain(V v) : SV_TARGET { return image.SampleLevel(linearClamp, v.uv, 0); }
        )";
        Microsoft::WRL::ComPtr<ID3DBlob> vs, ps, errors;
        HRESULT hr = D3DCompile(shader, sizeof(shader) - 1, nullptr, nullptr, nullptr,
            "VSMain", "vs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &vs, &errors);
        if (SUCCEEDED(hr)) {
            errors.Reset();
            hr = D3DCompile(shader, sizeof(shader) - 1, nullptr, nullptr, nullptr,
                "PSMain", "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &ps, &errors);
        }
        if (FAILED(hr)) {
            error = errors ? std::string(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize())
                           : "preprocess shader compilation failed";
            return false;
        }
        if (FAILED(device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &vs_)) ||
            FAILED(device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &ps_))) {
            error = "preprocess shader creation failed"; return false;
        }
        D3D11_SAMPLER_DESC sampler = {};
        sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler.MaxLOD = D3D11_FLOAT32_MAX;
        D3D11_RASTERIZER_DESC raster = {};
        raster.FillMode = D3D11_FILL_SOLID;
        raster.CullMode = D3D11_CULL_NONE;
        raster.DepthClipEnable = TRUE;
        if (FAILED(device->CreateSamplerState(&sampler, &sampler_)) ||
            FAILED(device->CreateRasterizerState(&raster, &raster_))) {
            error = "preprocess state creation failed"; return false;
        }
        D3D11_TEXTURE2D_DESC desc = {};
        desc.Width = width; desc.Height = height;
        desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(device->CreateTexture2D(&desc, nullptr, &target_)) ||
            FAILED(device->CreateRenderTargetView(target_.Get(), nullptr, &rtv_)) ||
            FAILED(device->CreateShaderResourceView(target_.Get(), nullptr, &targetView_))) {
            error = "preprocess target creation failed"; return false;
        }
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(device->CreateTexture2D(&desc, nullptr, &staging_))) {
            error = "preprocess staging creation failed"; return false;
        }
        ResetCache();
        return true;
    }

    void ResetCache() { cachedView_.Reset(); cachedPixels_.clear(); cachedTime_ = 0; }
    uint64_t CacheHits() const { return cacheHits_; }
    uint64_t Readbacks() const { return readbacks_; }

    // GPU-only diagnostic resize. The returned view is transient and is valid
    // until the next Resize/Read. Never retain it in the capture timeline.
    ID3D11ShaderResourceView* OutputView() const { return targetView_.Get(); }

    bool Read(ID3D11DeviceContext* context, ID3D11ShaderResourceView* view,
              int64_t captureTime, std::vector<uint8_t>& pixels) {
        if (!context || !view || !staging_) return false;
        // Holding the SRV prevents address reuse; timestamp protects against
        // accidental reuse of a view across different captures.
        if (view == cachedView_.Get() && captureTime == cachedTime_) {
            pixels = cachedPixels_;
            ++cacheHits_;
            return true;
        }
        if (!Resize(context, view)) return false;
        context->CopyResource(staging_.Get(), target_.Get());
        D3D11_MAPPED_SUBRESOURCE mapped = {};
        if (FAILED(context->Map(staging_.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return false;
        pixels.resize(static_cast<size_t>(width_) * height_ * 4);
        for (UINT y = 0; y < height_; ++y)
            std::memcpy(pixels.data() + static_cast<size_t>(y) * width_ * 4,
                static_cast<const uint8_t*>(mapped.pData) + static_cast<size_t>(y) * mapped.RowPitch,
                static_cast<size_t>(width_) * 4);
        context->Unmap(staging_.Get(), 0);
        cachedView_ = view;
        cachedTime_ = captureTime;
        cachedPixels_ = pixels;
        ++readbacks_;
        return true;
    }

    bool Resize(ID3D11DeviceContext* context, ID3D11ShaderResourceView* view) {
        if (!context || !view || !rtv_ || view == targetView_.Get()) return false;
        // Unbind the prior display SRV before rebinding our target as an RTV.
        ID3D11ShaderResourceView* unbound = nullptr;
        context->PSSetShaderResources(0, 1, &unbound);
        D3D11_VIEWPORT viewport = { 0, 0, static_cast<float>(width_), static_cast<float>(height_), 0, 1 };
        context->OMSetRenderTargets(1, rtv_.GetAddressOf(), nullptr);
        context->OMSetBlendState(nullptr, nullptr, 0xffffffff);
        context->OMSetDepthStencilState(nullptr, 0);
        context->RSSetState(raster_.Get());
        context->RSSetViewports(1, &viewport);
        context->IASetInputLayout(nullptr);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->VSSetShader(vs_.Get(), nullptr, 0);
        context->GSSetShader(nullptr, nullptr, 0);
        context->PSSetShader(ps_.Get(), nullptr, 0);
        context->PSSetSamplers(0, 1, sampler_.GetAddressOf());
        context->PSSetShaderResources(0, 1, &view);
        context->Draw(3, 0);
        context->PSSetShaderResources(0, 1, &unbound);
        context->OMSetRenderTargets(0, nullptr, nullptr);
        context->RSSetState(nullptr);
        return true;
    }

    // Compact BGRA -> RGB NCHW, no spatial interpolation on the worker.
    static void PackRgb(const std::vector<uint8_t>& pixels, std::vector<float>& rgb) {
        const size_t plane = pixels.size() / 4;
        rgb.resize(plane * 3);
        for (size_t i = 0; i < plane; ++i) {
            rgb[i] = pixels[i * 4 + 2] / 255.0f;
            rgb[plane + i] = pixels[i * 4 + 1] / 255.0f;
            rgb[plane * 2 + i] = pixels[i * 4] / 255.0f;
        }
    }
private:
    UINT width_ = 0, height_ = 0;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> vs_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> ps_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler_;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> raster_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> target_, staging_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> targetView_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> cachedView_;
    std::vector<uint8_t> cachedPixels_;
    int64_t cachedTime_ = 0;
    uint64_t cacheHits_ = 0, readbacks_ = 0;
};