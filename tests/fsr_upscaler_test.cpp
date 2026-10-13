#define NOMINMAX
#include "../src/fsr_upscaler.h"
#include "../src/upscale_comparison.h"
#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <vector>

using Microsoft::WRL::ComPtr;
static void Check(bool ok, const char* message) {
    if (!ok) { std::cerr << message << '\n'; std::exit(1); }
}
using Pixel = std::array<float, 4>;

int main(int argc, char** argv) {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    Check(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
        &device, nullptr, &context)), "create WARP device");
    wchar_t exe[32768] = {};
    Check(GetModuleFileNameW(nullptr, exe, 32768) != 0, "executable path");
    const auto shader = std::filesystem::path(exe).parent_path() / "shaders" / "fsr1.hlsl";
    FsrUpscaler scaler;
    std::string error;
    const bool initialized = scaler.Initialize(device.Get(), shader.wstring(), error);
    Check(initialized, error.c_str());
    constexpr UINT sw = 64, sh = 48;
    std::vector<uint8_t> source(sw * sh * 4);
    ComPtr<ID3D11Texture2D> input;
    ComPtr<ID3D11ShaderResourceView> view;
    auto upload = [&]() {
        view.Reset(); input.Reset();
        D3D11_TEXTURE2D_DESC d = {};
        d.Width = sw; d.Height = sh; d.MipLevels = d.ArraySize = d.SampleDesc.Count = 1;
        d.Format = DXGI_FORMAT_B8G8R8A8_UNORM; d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA data = {source.data(), sw * 4, 0};
        Check(SUCCEEDED(device->CreateTexture2D(&d, &data, &input)), "create source");
        Check(SUCCEEDED(device->CreateShaderResourceView(input.Get(), nullptr, &view)), "source SRV");
    };
    auto render = [&](UINT w, UINT h, bool easu, float sharpness) {
        D3D11_TEXTURE2D_DESC d = {};
        d.Width = w; d.Height = h; d.MipLevels = d.ArraySize = d.SampleDesc.Count = 1;
        // Float output exposes NaN/Inf that UNORM conversion could conceal.
        d.Format = DXGI_FORMAT_R32G32B32A32_FLOAT; d.BindFlags = D3D11_BIND_RENDER_TARGET;
        ComPtr<ID3D11Texture2D> output, staging;
        ComPtr<ID3D11RenderTargetView> target;
        Check(SUCCEEDED(device->CreateTexture2D(&d, nullptr, &output)), "output");
        Check(SUCCEEDED(device->CreateRenderTargetView(output.Get(), nullptr, &target)), "RTV");
        Check(!scaler.Draw(context.Get(), nullptr, target.Get(), w, h, easu, sharpness), "reject null source");
        Check(!scaler.Draw(context.Get(), view.Get(), target.Get(), 0, h, easu, sharpness), "reject zero size");
        Check(!scaler.Draw(context.Get(), view.Get(), target.Get(), w, h, easu,
            std::numeric_limits<float>::quiet_NaN()), "reject NaN strength");
        Check(scaler.Draw(context.Get(), view.Get(), target.Get(), w, h, easu, sharpness), "FSR draw");
        d.BindFlags = 0; d.Usage = D3D11_USAGE_STAGING; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        Check(SUCCEEDED(device->CreateTexture2D(&d, nullptr, &staging)), "staging");
        context->CopyResource(staging.Get(), output.Get());
        D3D11_MAPPED_SUBRESOURCE mapped = {};
        Check(SUCCEEDED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)), "readback");
        std::vector<Pixel> pixels(w * h);
        for (UINT y = 0; y < h; ++y) {
            auto row = reinterpret_cast<const Pixel*>(static_cast<const uint8_t*>(mapped.pData) + y * mapped.RowPitch);
            std::copy_n(row, w, pixels.data() + y * w);
        }
        context->Unmap(staging.Get(), 0);
        for (const auto& p : pixels) for (float c : p)
            Check(std::isfinite(c) && c >= 0 && c <= 1.00001f, "finite bounded output");
        for (const auto& p : pixels) Check(p[3] == 1, "opaque alpha");
        return pixels;
    };
    // Constant black/white and unequal RGB exercise borders, channel order and RCAS singularities.
    for (auto color : {std::array<uint8_t,3>{0,0,0}, {255,255,255}, {39,117,203}}) {
        for (size_t i = 0; i < source.size(); i += 4) {
            for (size_t c = 0; c < 3; ++c) source[i+c] = color[c];
            source[i+3] = 255;
        }
        upload();
        for (bool easu : {false, true}) for (float sharp : {0.f, .6f, 1.f}) {
            const auto pixels = render(97, 73, easu, sharp);
            for (const auto& p : pixels) for (size_t c = 0; c < 3; ++c) {
                // Reference RCAS uses approximate reciprocals; allow one 8-bit level.
                if (std::abs(p[c] - color[2-c]/255.f) >= 1.f/255.f)
                    std::cerr << "constant: easu=" << easu << " sharp=" << sharp << " channel=" << c
                        << " got=" << p[c] << " expected=" << color[2-c]/255.f << '\n';
                Check(std::abs(p[c] - color[2-c]/255.f) < 1.f/255.f, "constant RGB/border preservation");
            }
        }
    }
    // One frozen pattern for all comparisons: gradients, diagonal edges and thin strokes.
    for (UINT y = 0; y < sh; ++y) for (UINT x = 0; x < sw; ++x) {
        const size_t i = (y*sw+x)*4;
        source[i] = uint8_t(x * 255 / (sw-1));
        source[i+1] = uint8_t(y * 255 / (sh-1));
        source[i+2] = uint8_t((x > y || x % 11 == 0) ? 210 : 35);
        source[i+3] = 255;
    }
    upload();
    auto identity = render(sw, sh, true, 0);
    for (size_t i = 0; i < identity.size(); ++i) for (size_t c = 0; c < 3; ++c)
        Check(std::abs(identity[i][c] - source[i*4+2-c]/255.f) < .0001f, "identity/orientation/channel parity");
    const UINT w = 96, h = 72;
    auto bilinear = render(w, h, false, 0);
    auto easu = render(w, h, true, 0);
    auto sharpened = render(w, h, true, .6f);
    auto bilinearSharp = render(w, h, false, .6f);
    auto different = [](const std::vector<Pixel>& a, const std::vector<Pixel>& b) {
        double sum = 0;
        for (size_t i=0; i<a.size(); ++i) for (size_t c=0;c<3;++c) sum += std::abs(a[i][c]-b[i][c]);
        return sum / (a.size()*3);
    };
    Check(different(bilinear, easu) > .0005, "EASU is not a bilinear no-op");
    Check(different(easu, sharpened) > .0001, "RCAS changes nonconstant image");
    Check(different(bilinear, bilinearSharp) > .0001, "RCAS independent of EASU switch");
    Check(different(sharpened, render(w,h,true,.6f)) == 0, "same frozen input deterministic");
    auto downsampled = render(31,23,true,1);
    Check(different(downsampled, render(31,23,false,0)) == 0, "downscale bypasses EASU/RCAS");
    // Independent CPU bilinear reference at non-integral scale and clamped borders.
    for (UINT y=0;y<h;++y) for (UINT x=0;x<w;++x) {
        float fx=std::clamp((x+.5f)*sw/w-.5f,0.f,float(sw-1));
        float fy=std::clamp((y+.5f)*sh/h-.5f,0.f,float(sh-1));
        UINT x0=UINT(fx), y0=UINT(fy), x1=std::min(x0+1,sw-1), y1=std::min(y0+1,sh-1);
        for (UINT c=0;c<3;++c) {
            auto s=[&](UINT xx,UINT yy){return source[(yy*sw+xx)*4+2-c]/255.f;};
            float top=s(x0,y0)*(1-(fx-x0))+s(x1,y0)*(fx-x0);
            float bottom=s(x0,y1)*(1-(fx-x0))+s(x1,y1)*(fx-x0);
            Check(std::abs(bilinear[y*w+x][c]-(top*(1-(fy-y0))+bottom*(fy-y0))) < .006f, "CPU bilinear reference");
        }
    }
    if (argc == 2) {
        std::filesystem::path dir(argv[1]); std::filesystem::create_directories(dir);
        auto save=[&](const char* name,const std::vector<Pixel>& pixels) {
            std::ofstream f(dir/name,std::ios::binary); f << "P6\n" << w << ' ' << h << "\n255\n";
            for(const auto& p:pixels) for(size_t c=0;c<3;++c) f.put(char(std::lround(p[c]*255)));
            Check(bool(f),"save frozen comparison");
        };
        save("bilinear.ppm",bilinear); save("easu.ppm",easu); save("easu_rcas.ppm",sharpened);
    }
    // Export plumbing test. This callback substitutes bilinear for legacy:
    // it tests dispatch, files, one frozen source and common RCAS, NOT legacy parity.
    const auto directory = std::filesystem::temp_directory_path() /
        ("aiframe-export-test-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64()));
    ComPtr<ID3D11Resource> frozenIdentity;
    int legacyCalls = 0;
    UpscaleComparison::LegacyDraw legacy = [&](auto* src, auto* dst, UINT dw, UINT dh, bool cubic, float gain) {
        ComPtr<ID3D11Resource> r; src->GetResource(&r);
        Check(r.Get() != input.Get(), "export owns a frozen copy, not live source");
        if (frozenIdentity) Check(r.Get() == frozenIdentity.Get(), "every variant uses same frozen source");
        else frozenIdentity = r;
        Check(cubic == (legacyCalls != 0), "legacy bilinear/bicubic dispatch");
        Check(gain == (legacyCalls == 2 ? .6f : 0.f), "legacy comparison gain dispatch");
        ++legacyCalls;
        Check(scaler.Draw(context.Get(), src, dst, dw, dh, false, gain), "export callback draw");
    };
    Check(UpscaleComparison::Export(device.Get(), context.Get(), view.Get(), w, h, .6f,
        123, 456, shader, directory, legacy, error), error.c_str());
    Check(legacyCalls == 3, "legacy callback count");
    auto read = [&](const char* name) {
        std::ifstream f(directory / name, std::ios::binary);
        Check(bool(f), "comparison file exists");
        return std::vector<char>(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    };
    const auto bmp = read("source.bmp");
    BITMAPFILEHEADER bf = {}; BITMAPINFOHEADER bi = {};
    Check(bmp.size() == sizeof(bf) + sizeof(bi) + source.size(), "BMP file length");
    std::memcpy(&bf, bmp.data(), sizeof(bf));
    std::memcpy(&bi, bmp.data() + sizeof(bf), sizeof(bi));
    Check(bf.bfType == 0x4d42 && bf.bfSize == bmp.size() && bi.biWidth == sw &&
        bi.biHeight == -LONG(sh) && bi.biBitCount == 32, "BMP top-down dimensions and header");
    Check(std::memcmp(bmp.data() + bf.bfOffBits, source.data(), source.size()) == 0,
        "frozen bitmap exact BGRA pixels and row orientation");
    Check(read("bilinear.bmp") == read("bicubic.bmp"), "stub callback produces identical base output");
    Check(read("bilinear_rcas.bmp") == read("bicubic_rcas.bmp"), "common sharpening treats equal inputs equally");
    Check(read("easu.bmp") != read("easu_rcas.bmp"), "exported RCAS changes pattern");
    Check(read("legacy_bicubic_sharpen.bmp").size() == 54 + w*h*4, "legacy export dimensions");
    const auto manifest = read("manifest.txt");
    Check(std::string(manifest.begin(), manifest.end()).find("pair=123") != std::string::npos, "manifest source identity");
    Check(!read("compare.html").empty(), "comparison viewer written");
    Check(!UpscaleComparison::Export(device.Get(), context.Get(), view.Get(), w, h, .6f,
        123, 456, shader, directory, legacy, error), "export refuses to overwrite previous comparison");
    std::filesystem::remove_all(directory);
    std::cout << "FSR shader and frozen-export file/dispatch tests passed (legacy callback parity not covered).\n";
}