#pragma once
#include <d3d11.h>
#include <wrl/client.h>
#include <cstdint>

// Render-thread only. Source views must refer to immutable frame content.
// Store the FINAL quantized display pixels: hits cannot change filtering or color.
class DisplayCache {
public:
    void Invalidate() { source_.Reset(); }
    bool Restore(ID3D11DeviceContext* context, ID3D11Texture2D* target,
        ID3D11ShaderResourceView* source, uint64_t revision) {
        if (!source || source_.Get() != source || revision_ != revision || !Compatible(target)) return false;
        context->OMSetRenderTargets(0, nullptr, nullptr);
        context->CopyResource(target, image_.Get());
        return true;
    }
    bool Store(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Texture2D* target,
        ID3D11ShaderResourceView* source, uint64_t revision) {
        Invalidate();
        if (!target || !source) return false;
        if (!Compatible(target)) {
            D3D11_TEXTURE2D_DESC desc = {};
            target->GetDesc(&desc);
            desc.Usage = D3D11_USAGE_DEFAULT;
            desc.BindFlags = desc.CPUAccessFlags = desc.MiscFlags = 0;
            image_.Reset();
            if (FAILED(device->CreateTexture2D(&desc, nullptr, &image_))) return false;
        }
        context->OMSetRenderTargets(0, nullptr, nullptr);
        context->CopyResource(image_.Get(), target);
        source_ = source; // retain identity: allocator cannot recycle this pointer
        revision_ = revision;
        return true;
    }
private:
    bool Compatible(ID3D11Texture2D* target) const {
        if (!image_ || !target) return false;
        D3D11_TEXTURE2D_DESC a = {}, b = {};
        image_->GetDesc(&a); target->GetDesc(&b);
        return a.Width == b.Width && a.Height == b.Height && a.Format == b.Format &&
            a.MipLevels == b.MipLevels && a.ArraySize == b.ArraySize &&
            a.SampleDesc.Count == b.SampleDesc.Count && a.SampleDesc.Quality == b.SampleDesc.Quality;
    }
    Microsoft::WRL::ComPtr<ID3D11Texture2D> image_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> source_;
    uint64_t revision_ = 0;
};