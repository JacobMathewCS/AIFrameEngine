#include "../src/display_cache.h"
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <vector>
using Microsoft::WRL::ComPtr;
static void Check(bool ok, const char* message) {
    if (!ok) { std::cerr << message << '\n'; std::exit(1); }
}
int main() {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    Check(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0,
        nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context)), "device");
    auto texture = [&](UINT width, UINT height, DXGI_FORMAT format) {
        D3D11_TEXTURE2D_DESC d = {};
        d.Width = width; d.Height = height; d.MipLevels = d.ArraySize = 1;
        d.Format = format; d.SampleDesc.Count = 1;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        ComPtr<ID3D11Texture2D> t;
        Check(SUCCEEDED(device->CreateTexture2D(&d, nullptr, &t)), "texture");
        return t;
    };
    auto target = texture(37, 19, DXGI_FORMAT_R8G8B8A8_UNORM);
    auto source = texture(7, 5, DXGI_FORMAT_R8G8B8A8_UNORM);
    ComPtr<ID3D11ShaderResourceView> view, other;
    Check(SUCCEEDED(device->CreateShaderResourceView(source.Get(), nullptr, &view)), "view");
    Check(SUCCEEDED(device->CreateShaderResourceView(source.Get(), nullptr, &other)), "other view");
    DisplayCache cache;
    Check(!cache.Restore(context.Get(), target.Get(), view.Get(), 1), "empty miss");
    std::vector<unsigned char> pixels(37 * 19 * 4), zeros(pixels.size());
    for (size_t i = 0; i < pixels.size(); ++i) pixels[i] = static_cast<unsigned char>(i * 37 + i / 11);
    context->UpdateSubresource(target.Get(), 0, nullptr, pixels.data(), 37 * 4, 0);
    Check(cache.Store(device.Get(), context.Get(), target.Get(), view.Get(), 1), "store");
    Check(!cache.Restore(context.Get(), target.Get(), other.Get(), 1), "new frame miss");
    Check(!cache.Restore(context.Get(), target.Get(), view.Get(), 2), "settings miss");
    auto resized = texture(38, 19, DXGI_FORMAT_R8G8B8A8_UNORM);
    auto reformatted = texture(37, 19, DXGI_FORMAT_B8G8R8A8_UNORM);
    Check(!cache.Restore(context.Get(), resized.Get(), view.Get(), 1), "size miss");
    Check(!cache.Restore(context.Get(), reformatted.Get(), view.Get(), 1), "format miss");
    D3D11_TEXTURE2D_DESC d = {}; target->GetDesc(&d);
    d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    Check(SUCCEEDED(device->CreateTexture2D(&d, nullptr, &staging)), "staging");
    for (int repeat = 0; repeat < 4; ++repeat) {
        context->UpdateSubresource(target.Get(), 0, nullptr, zeros.data(), 37 * 4, 0);
        Check(cache.Restore(context.Get(), target.Get(), view.Get(), 1), "hit");
        context->CopyResource(staging.Get(), target.Get());
        D3D11_MAPPED_SUBRESOURCE mapped = {};
        Check(SUCCEEDED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)), "map");
        for (UINT y = 0; y < 19; ++y)
            Check(std::memcmp(static_cast<unsigned char*>(mapped.pData) + y * mapped.RowPitch,
                pixels.data() + y * 37 * 4, 37 * 4) == 0, "exact final pixel parity");
        context->Unmap(staging.Get(), 0);
    }
    cache.Invalidate();
    Check(!cache.Restore(context.Get(), target.Get(), view.Get(), 1), "explicit invalidation");
    std::vector<unsigned char> resizedPixels(38 * 19 * 4, 71);
    context->UpdateSubresource(resized.Get(), 0, nullptr, resizedPixels.data(), 38 * 4, 0);
    Check(cache.Store(device.Get(), context.Get(), resized.Get(), other.Get(), 2), "resize recreation");
    Check(cache.Restore(context.Get(), resized.Get(), other.Get(), 2), "resized hit");
    Check(!cache.Store(device.Get(), context.Get(), nullptr, view.Get(), 2), "null store");
    Check(!cache.Restore(context.Get(), resized.Get(), other.Get(), 2), "failed store invalidates");
    std::cout << "PASS: exact display pixels, repeated hits, source/settings/size/format invalidation\n";
}