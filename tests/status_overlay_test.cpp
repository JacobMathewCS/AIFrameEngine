#include "../src/status_overlay.h"
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
    StatusRate rate;
    rate.Observe(100, 50); rate.Observe(10000100, 80);
    Check(rate.Value() == 30, "interval rate not cumulative count");
    rate.Reset(); Check(rate.Value() == 0, "toggle clears old rate");
    rate.Observe(100, 80); rate.Observe(20000100, 80);
    Check(rate.Value() == 0, "idle reports zero");
    rate.Observe(30000100, 110); Check(rate.Value() == 30, "recovery");
    rate.Observe(40000100, 1); Check(rate.Value() == 0, "counter reset");
    rate.Observe(100, 1); Check(rate.Value() == 0, "clock reversal");
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    Check(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0,
        nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context)), "WARP device");
    D3D11_TEXTURE2D_DESC d = {};
    d.Width = 800; d.Height = 400; d.MipLevels = d.ArraySize = d.SampleDesc.Count = 1;
    d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> target;
    Check(SUCCEEDED(device->CreateTexture2D(&d, nullptr, &target)), "target");
    ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<ID3D11ShaderResourceView> srv;
    Check(SUCCEEDED(device->CreateRenderTargetView(target.Get(), nullptr, &rtv)), "RTV");
    Check(SUCCEEDED(device->CreateShaderResourceView(target.Get(), nullptr, &srv)), "SRV");
    const float clear[] = {0, 0, 0, 1};
    context->ClearRenderTargetView(rtv.Get(), clear);
    DisplayCache cache;
    Check(cache.Store(device.Get(), context.Get(), target.Get(), srv.Get(), 0), "store clean game image");
    d.BindFlags = 0; d.Usage = D3D11_USAGE_STAGING; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    Check(SUCCEEDED(device->CreateTexture2D(&d, nullptr, &staging)), "staging");
    auto pixels = [&]() {
        context->CopyResource(staging.Get(), target.Get());
        D3D11_MAPPED_SUBRESOURCE m = {};
        Check(SUCCEEDED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m)), "read pixels");
        std::vector<unsigned char> out(d.Width * d.Height * 4);
        for (UINT y = 0; y < d.Height; ++y)
            std::memcpy(out.data() + y * d.Width * 4, static_cast<unsigned char*>(m.pData) + y * m.RowPitch, d.Width * 4);
        context->Unmap(staging.Get(), 0); return out;
    };
    const auto clean = pixels();
    StatusOverlay overlay;
    Check(overlay.Initialize(device.Get(), context.Get()), "overlay init");
    StatusSnapshot s; s.target = "Test game"; s.scaler = "EASU"; s.fgState = "ON";
    s.displayMode = "normal"; s.sourceWidth = 1280; s.sourceHeight = 720;
    s.generatedRate = 29.5; s.bufferMs = 49;
    for (int i = 0; i < 3; ++i) {
        Check(cache.Restore(context.Get(), target.Get(), srv.Get(), 0), "restore without UI");
        Check(overlay.Draw(context.Get(), rtv.Get(), d.Width, d.Height, 1000000 + i * 166667, s), "draw");
    }
    Check(pixels() != clean, "overlay produces visible pixels");
    Check(cache.Restore(context.Get(), target.Get(), srv.Get(), 0), "hide panel");
    Check(pixels() == clean, "hidden panel leaves no cached trails");
    overlay.Shutdown(); overlay.Shutdown();
    Check(overlay.Initialize(device.Get(), context.Get()), "reinitialize");
    Check(!overlay.Draw(context.Get(), nullptr, 0, 0, 0, s), "invalid target rejected");
    std::cout << "PASS: rate/reset, WARP overlay draw, clean cache restore, shutdown/reinit\n";
}