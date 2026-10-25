#pragma once
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <string>
#include <cmath>

// Render-thread only. Two GPU passes, no readback. SDR UNORM source/output.
// Changes graphics state: callers must rebind their own pipeline afterwards.
class FsrUpscaler {
    template<class T> using Ptr = Microsoft::WRL::ComPtr<T>;
public:
    bool Initialize(ID3D11Device* device, const std::wstring& shader, std::string& error) {
        if (!device) { error = "null FSR device"; return false; }
        device_ = device;
        auto compile = [&](const char* entry, const char* profile, const char* define, Ptr<ID3DBlob>& code) {
            D3D_SHADER_MACRO macros[] = {{define, "1"}, {nullptr, nullptr}};
            Ptr<ID3DBlob> errors;
            HRESULT hr = D3DCompileFromFile(shader.c_str(), define ? macros : nullptr,
                D3D_COMPILE_STANDARD_FILE_INCLUDE, entry, profile,
                D3DCOMPILE_OPTIMIZATION_LEVEL3 | D3DCOMPILE_ENABLE_STRICTNESS, 0, &code, &errors);
            if (FAILED(hr)) error = errors ? std::string(static_cast<const char*>(errors->GetBufferPointer()),
                errors->GetBufferSize()) : "FSR shader file unavailable";
            return SUCCEEDED(hr);
        };
        Ptr<ID3DBlob> vs, easu, rcas, copy;
        if (!compile("VSMain", "vs_5_0", nullptr, vs) ||
            !compile("PSMain", "ps_5_0", "EASU_PASS", easu) ||
            !compile("PSMain", "ps_5_0", "RCAS_PASS", rcas) ||
            !compile("PSMain", "ps_5_0", nullptr, copy)) return false;
        if (FAILED(device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &vs_)) ||
            FAILED(device->CreatePixelShader(easu->GetBufferPointer(), easu->GetBufferSize(), nullptr, &easu_)) ||
            FAILED(device->CreatePixelShader(rcas->GetBufferPointer(), rcas->GetBufferSize(), nullptr, &rcas_)) ||
            FAILED(device->CreatePixelShader(copy->GetBufferPointer(), copy->GetBufferSize(), nullptr, &copy_))) {
            error = "FSR shader creation failed"; return false;
        }
        D3D11_BUFFER_DESC cb = {};
        cb.ByteWidth = sizeof(Params); cb.Usage = D3D11_USAGE_DEFAULT; cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        D3D11_SAMPLER_DESC sampler = {};
        sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler.MaxLOD = D3D11_FLOAT32_MAX;
        D3D11_RASTERIZER_DESC raster = {};
        raster.FillMode = D3D11_FILL_SOLID; raster.CullMode = D3D11_CULL_NONE; raster.DepthClipEnable = TRUE;
        if (FAILED(device->CreateBuffer(&cb, nullptr, &constants_)) ||
            FAILED(device->CreateSamplerState(&sampler, &sampler_)) ||
            FAILED(device->CreateRasterizerState(&raster, &raster_))) {
            error = "FSR state creation failed"; return false;
        }
        return true;
    }

    bool Draw(ID3D11DeviceContext* context, ID3D11ShaderResourceView* source,
        ID3D11RenderTargetView* destination, UINT width, UINT height, bool useEasu, float strength,
        bool nativeSizeFastPath = true) {
        if (!context || !source || !destination || !width || !height || !constants_ || !std::isfinite(strength)) return false;
        Ptr<ID3D11Resource> resource; source->GetResource(&resource);
        Ptr<ID3D11Texture2D> texture;
        if (FAILED(resource.As(&texture))) return false;
        D3D11_TEXTURE2D_DESC desc = {}; texture->GetDesc(&desc);
        // Never use EASU as a downsampling filter. Native-size is a copy + optional RCAS.
        const bool upscale = width >= desc.Width && height >= desc.Height;
        const bool easu = useEasu && upscale && (width > desc.Width || height > desc.Height);
        const bool native = nativeSizeFastPath && width == desc.Width && height == desc.Height;
        Params p = {{float(desc.Width), float(desc.Height)}, {float(width), float(height)},
            upscale ? (strength < 0 ? 0 : (strength > 1 ? 1 : strength)) : 0, {0, 0, 0}};
        // RCAS already loads output-space texels. At 1:1 the source is that
        // image: avoid a full-resolution copy pass and intermediate allocation.
        // The optional old path is retained for numeric regression comparisons.
        if (p.strength > 0 && !native && !EnsureTarget(width, height)) return false;
        context->UpdateSubresource(constants_.Get(), 0, nullptr, &p, 0, 0);
        D3D11_VIEWPORT viewport = {0, 0, float(width), float(height), 0, 1};
        context->RSSetViewports(1, &viewport); context->RSSetState(raster_.Get());
        context->OMSetBlendState(nullptr, nullptr, 0xffffffff); context->OMSetDepthStencilState(nullptr, 0);
        context->IASetInputLayout(nullptr); context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->VSSetShader(vs_.Get(), nullptr, 0); context->GSSetShader(nullptr, nullptr, 0);
        context->PSSetSamplers(0, 1, sampler_.GetAddressOf());
        context->PSSetConstantBuffers(0, 1, constants_.GetAddressOf());
        auto pass = [&](ID3D11ShaderResourceView* input, ID3D11RenderTargetView* target, ID3D11PixelShader* shader) {
            ID3D11ShaderResourceView* empty = nullptr;
            context->PSSetShaderResources(0, 1, &empty);
            context->OMSetRenderTargets(1, &target, nullptr);
            context->PSSetShaderResources(0, 1, &input);
            context->PSSetShader(shader, nullptr, 0); context->Draw(3, 0);
            context->PSSetShaderResources(0, 1, &empty);
            context->OMSetRenderTargets(0, nullptr, nullptr);
        };
        if (p.strength > 0 && native) {
            pass(source, destination, rcas_.Get());
        } else if (p.strength > 0) {
            pass(source, intermediateRtv_.Get(), easu ? easu_.Get() : copy_.Get());
            pass(intermediateView_.Get(), destination, rcas_.Get());
        } else pass(source, destination, easu ? easu_.Get() : copy_.Get());
        context->RSSetState(nullptr);
        return true;
    }
private:
    struct Params { float input[2], output[2], strength, padding[3]; };
    bool EnsureTarget(UINT width, UINT height) {
        if (width == width_ && height == height_ && intermediateView_) return true;
        intermediateView_.Reset(); intermediateRtv_.Reset(); intermediate_.Reset();
        D3D11_TEXTURE2D_DESC d = {};
        d.Width = width; d.Height = height; d.MipLevels = d.ArraySize = d.SampleDesc.Count = 1;
        d.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(device_->CreateTexture2D(&d, nullptr, &intermediate_)) ||
            FAILED(device_->CreateRenderTargetView(intermediate_.Get(), nullptr, &intermediateRtv_)) ||
            FAILED(device_->CreateShaderResourceView(intermediate_.Get(), nullptr, &intermediateView_))) return false;
        width_ = width; height_ = height; return true;
    }
    UINT width_ = 0, height_ = 0;
    Ptr<ID3D11Device> device_;
    Ptr<ID3D11VertexShader> vs_;
    Ptr<ID3D11PixelShader> easu_, rcas_, copy_;
    Ptr<ID3D11Buffer> constants_;
    Ptr<ID3D11SamplerState> sampler_;
    Ptr<ID3D11RasterizerState> raster_;
    Ptr<ID3D11Texture2D> intermediate_;
    Ptr<ID3D11RenderTargetView> intermediateRtv_;
    Ptr<ID3D11ShaderResourceView> intermediateView_;
};