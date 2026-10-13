#define NOMINMAX
#include "../src/rife_preprocessor.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>

static void Check(bool ok, const char* message) {
    if (!ok) { std::cerr << message << '\n'; std::exit(1); }
}

int main() {
    using Microsoft::WRL::ComPtr;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    Check(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
        &device, nullptr, &context)), "create WARP test device");
    RifePreprocessor processor;
    std::string error;
    Check(!processor.Initialize(device.Get(), 0, 3, error), "reject empty size");
    // Odd output width exercises RowPitch handling; nonintegral scale exercises
    // half-pixel alignment, edge clamping and channel order.
    constexpr UINT sw = 13, sh = 7, dw = 5, dh = 3;
    Check(processor.Initialize(device.Get(), dw, dh, error), error.c_str());
    std::vector<uint8_t> source(sw * sh * 4);
    for (UINT y = 0; y < sh; ++y) for (UINT x = 0; x < sw; ++x) {
        const size_t p = (y * sw + x) * 4;
        source[p] = static_cast<uint8_t>((x * 19 + y * 11) % 256);
        source[p+1] = static_cast<uint8_t>(y * 37);
        source[p+2] = static_cast<uint8_t>(x * 17);
        source[p+3] = 255;
    }
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = sw; desc.Height = sh;
    desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA data = {source.data(), sw * 4, 0};
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11ShaderResourceView> view;
    Check(SUCCEEDED(device->CreateTexture2D(&desc, &data, &texture)), "source texture");
    Check(SUCCEEDED(device->CreateShaderResourceView(texture.Get(), nullptr, &view)), "source SRV");
    std::vector<uint8_t> output;
    Check(!processor.Read(context.Get(), nullptr, 1, output), "reject null SRV");
    Check(processor.Read(context.Get(), view.Get(), 1, output), "GPU resize/readback");
    Check(output.size() == dw * dh * 4, "compact output size");
    double maxError = 0;
    for (UINT y = 0; y < dh; ++y) for (UINT x = 0; x < dw; ++x) {
        const float fx = std::clamp((x + .5f) * sw / dw - .5f, 0.f, float(sw-1));
        const float fy = std::clamp((y + .5f) * sh / dh - .5f, 0.f, float(sh-1));
        const UINT x0 = UINT(fx), y0 = UINT(fy);
        const UINT x1 = std::min(x0+1, sw-1), y1 = std::min(y0+1, sh-1);
        const float wx = fx-x0, wy = fy-y0;
        for (UINT c = 0; c < 4; ++c) {
            const float top = source[(y0*sw+x0)*4+c]*(1-wx) + source[(y0*sw+x1)*4+c]*wx;
            const float bottom = source[(y1*sw+x0)*4+c]*(1-wx) + source[(y1*sw+x1)*4+c]*wx;
            maxError = std::max(maxError, std::abs(double(output[(y*dw+x)*4+c]) - (top*(1-wy)+bottom*wy)));
        }
    }
    Check(maxError <= 2.0, "GPU/CPU bilinear parity within 2 byte levels");
    const auto expected = output;
    // The matched-resolution display uses GPU output directly, with no Map.
    Check(processor.Resize(context.Get(), view.Get()), "GPU-only comparison resize");
    Check(processor.Readbacks() == 1 && processor.OutputView(), "display resize adds no readback");
    Check(!processor.Resize(context.Get(), processor.OutputView()), "reject input/output alias");
    RifePreprocessor displayReadback;
    Check(displayReadback.Initialize(device.Get(), dw, dh, error), error.c_str());
    std::vector<uint8_t> displayPixels;
    Check(displayReadback.Read(context.Get(), processor.OutputView(), 1, displayPixels), "read GPU-only resize output");
    Check(displayPixels == expected, "matched display and inference resize parity");
    Check(processor.Read(context.Get(), view.Get(), 1, output), "cached frame");
    Check(output == expected && processor.CacheHits() == 1 && processor.Readbacks() == 1, "reuse exact previous capture");
    Check(processor.Read(context.Get(), view.Get(), 2, output), "new timestamp");
    Check(processor.Readbacks() == 2, "new timestamp cannot hit cache");
    processor.ResetCache();
    Check(processor.Read(context.Get(), view.Get(), 2, output), "reset cache");
    Check(processor.Readbacks() == 3, "reset forces readback");
    std::vector<float> rgb;
    RifePreprocessor::PackRgb(output, rgb);
    const size_t plane = dw * dh;
    Check(rgb.size() == plane*3, "tensor shape");
    for (size_t p = 0; p < plane; ++p) for (size_t c = 0; c < 3; ++c)
        Check(std::abs(rgb[c*plane+p] - output[p*4+2-c]/255.f) < 1.e-6f, "RGB NCHW packing");
    // Identity resize must preserve texels and orientation.
    Check(processor.Initialize(device.Get(), sw, sh, error), error.c_str());
    Check(processor.Read(context.Get(), view.Get(), 3, output), "identity resize");
    Check(output == source, "identity texel parity");
    std::cout << "GPU preprocessing passed; bilinear max byte error=" << maxError << '\n';
}