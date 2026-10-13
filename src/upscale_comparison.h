#pragma once
#include "fsr_upscaler.h"
#include <filesystem>
#include <fstream>
#include <functional>
#include <vector>
#include <cstdint>
#include <cstring>
#include <stdexcept>

// Explicit render-thread diagnostic. Performs blocking readbacks/file writes.
// Never call during a performance sample; caller rebinds graphics state afterward.
namespace UpscaleComparison {
using Microsoft::WRL::ComPtr;
using LegacyDraw = std::function<void(ID3D11ShaderResourceView*, ID3D11RenderTargetView*,
    UINT, UINT, bool, float)>;

inline void Require(HRESULT hr, const char* message) {
    if (FAILED(hr)) throw std::runtime_error(message);
}

inline void SaveBitmap(ID3D11Device* device, ID3D11DeviceContext* context,
    ID3D11Texture2D* texture, const std::filesystem::path& path) {
    D3D11_TEXTURE2D_DESC d = {}; texture->GetDesc(&d);
    if (d.Format != DXGI_FORMAT_B8G8R8A8_UNORM || d.SampleDesc.Count != 1 ||
        d.MipLevels != 1 || d.ArraySize != 1 || !d.Width || !d.Height ||
        uint64_t(d.Width) * d.Height * 4 > UINT32_MAX - 54)
        throw std::runtime_error("comparison bitmap requires a bounded BGRA8 texture");
    d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ; d.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> staging;
    Require(device->CreateTexture2D(&d, nullptr, &staging), "comparison staging allocation");
    context->CopyResource(staging.Get(), texture);
    // Allocate before Map so allocation exceptions cannot leave it mapped.
    std::vector<uint8_t> pixels(size_t(d.Width) * d.Height * 4);
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    Require(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "comparison readback");
    for (UINT y = 0; y < d.Height; ++y)
        std::memcpy(pixels.data() + size_t(y) * d.Width * 4,
            static_cast<const uint8_t*>(mapped.pData) + size_t(y) * mapped.RowPitch, size_t(d.Width) * 4);
    context->Unmap(staging.Get(), 0);

    BITMAPFILEHEADER file = {};
    BITMAPINFOHEADER info = {};
    file.bfType = 0x4d42;
    file.bfOffBits = sizeof(file) + sizeof(info);
    file.bfSize = file.bfOffBits + static_cast<DWORD>(pixels.size());
    info.biSize = sizeof(info); info.biWidth = d.Width;
    info.biHeight = -static_cast<LONG>(d.Height); // Top-down; BGRA rows, no inversion.
    info.biPlanes = 1; info.biBitCount = 32; info.biCompression = BI_RGB;
    info.biSizeImage = static_cast<DWORD>(pixels.size());
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(&file), sizeof(file));
    out.write(reinterpret_cast<const char*>(&info), sizeof(info));
    out.write(reinterpret_cast<const char*>(pixels.data()), pixels.size());
    out.close();
    if (!out) throw std::runtime_error("comparison bitmap write failed");
}

// All filters consume ONE immutable GPU copy of the client frame. Common RCAS
// comparison uses the same RGBA16F intermediate and the same output quantization.
inline bool Export(ID3D11Device* device, ID3D11DeviceContext* context,
    ID3D11ShaderResourceView* source, UINT width, UINT height, float gain,
    uint64_t pair, int64_t captureTime, const std::filesystem::path& shader,
    const std::filesystem::path& directory, const LegacyDraw& legacy, std::string& error) {
    try {
        if (!device || !context || !source || !width || !height || !legacy ||
            !std::isfinite(gain) || gain < 0 || gain > 1)
            throw std::runtime_error("invalid comparison parameters");
        ComPtr<ID3D11Resource> resource; source->GetResource(&resource);
        ComPtr<ID3D11Texture2D> original;
        Require(resource.As(&original), "comparison source is not a texture");
        D3D11_TEXTURE2D_DESC desc = {}; original->GetDesc(&desc);
        if (desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM || desc.MipLevels != 1 ||
            desc.ArraySize != 1 || desc.SampleDesc.Count != 1)
            throw std::runtime_error("comparison requires SDR BGRA8 client capture");
        FsrUpscaler fsr;
        if (!fsr.Initialize(device, shader.wstring(), error)) return false;
        if (!std::filesystem::create_directory(directory))
            throw std::runtime_error("comparison directory already exists or cannot be created");
        desc.Usage = D3D11_USAGE_DEFAULT; desc.CPUAccessFlags = desc.MiscFlags = 0;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        ComPtr<ID3D11Texture2D> frozen;
        ComPtr<ID3D11ShaderResourceView> frozenView;
        Require(device->CreateTexture2D(&desc, nullptr, &frozen), "frozen texture allocation");
        context->CopyResource(frozen.Get(), original.Get());
        Require(device->CreateShaderResourceView(frozen.Get(), nullptr, &frozenView), "frozen SRV");
        SaveBitmap(device, context, frozen.Get(), directory / "source.bmp");

        D3D11_TEXTURE2D_DESC output = {};
        output.Width = width; output.Height = height;
        output.MipLevels = output.ArraySize = output.SampleDesc.Count = 1;
        output.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        output.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        ComPtr<ID3D11Texture2D> base, result;
        ComPtr<ID3D11RenderTargetView> baseTarget, target;
        ComPtr<ID3D11ShaderResourceView> baseView;
        Require(device->CreateTexture2D(&output, nullptr, &base), "comparison base allocation");
        Require(device->CreateRenderTargetView(base.Get(), nullptr, &baseTarget), "comparison base RTV");
        Require(device->CreateShaderResourceView(base.Get(), nullptr, &baseView), "comparison base SRV");
        output.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        Require(device->CreateTexture2D(&output, nullptr, &result), "comparison result allocation");
        Require(device->CreateRenderTargetView(result.Get(), nullptr, &target), "comparison result RTV");
        for (int filter = 0; filter < 3; ++filter) {
            const std::string name = filter == 0 ? "bilinear" : filter == 1 ? "bicubic" : "easu";
            if (filter < 2) legacy(frozenView.Get(), baseTarget.Get(), width, height, filter == 1, 0);
            else if (!fsr.Draw(context, frozenView.Get(), baseTarget.Get(), width, height, true, 0))
                throw std::runtime_error("comparison EASU draw failed");
            context->OMSetRenderTargets(0, nullptr, nullptr);
            for (int sharpen = 0; sharpen < 2; ++sharpen) {
                if (!fsr.Draw(context, baseView.Get(), target.Get(), width, height, false, sharpen ? gain : 0))
                    throw std::runtime_error("comparison common RCAS draw failed");
                SaveBitmap(device, context, result.Get(), directory / (name + (sharpen ? "_rcas.bmp" : ".bmp")));
            }
        }
        // Actual legacy one-pass sharpen is deliberately separated from the fair
        // common-sharpening comparison; its gain is not numerically equivalent.
        legacy(frozenView.Get(), target.Get(), width, height, true, gain);
        context->OMSetRenderTargets(0, nullptr, nullptr);
        SaveBitmap(device, context, result.Get(), directory / "legacy_bicubic_sharpen.bmp");

        std::ofstream meta(directory / "manifest.txt");
        meta << "Same frozen SDR client frame; no generated frame.\npair=" << pair
            << "\ncapture_time_100ns=" << captureTime << "\nsource=" << desc.Width << 'x' << desc.Height
            << "\noutput=" << width << 'x' << height << "\ncommon_rcas_gain=" << gain
            << "\nCommon comparison: bilinear/bicubic/EASU -> RGBA16F -> reference RCAS -> BGRA8."
            << "\nUnsharpened files bypass RCAS. Gain 0 makes both sets identical."
            << "\nLegacy sharpen file uses the actual custom one-pass implementation, not common RCAS."
            << "\nNo native-resolution ground truth; differences do not establish which is more accurate."
            << "\nExport is blocking and NOT a performance sample.\n";
        meta.close(); if (!meta) throw std::runtime_error("comparison manifest write failed");
        std::ofstream html(directory / "compare.html");
        html << R"HTML(<!doctype html><meta charset="utf-8"><title>Same-frame upscale comparison</title>
<style>body{background:#202124;color:#eee;font:16px sans-serif}button,select{font:inherit;margin:6px}
#viewport{overflow:auto;height:75vh;border:1px solid #aaa}img{display:block;max-width:none} .row{display:flex;gap:12px}.pane{overflow:auto;max-width:48vw} .pane img{max-width:none}</style>
<h1>Same frozen game frame</h1><p>View at browser zoom 100%. Main image uses output pixels 1:1. Switch filters without moving the image. No inferred quality score.</p>
<label>Sharpening <select id="sharp"><option value="">Off</option><option value="_rcas">Common reference RCAS</option></select></label>
<button onclick="pick('bilinear')">Bilinear</button><button onclick="pick('bicubic')">Bicubic</button><button onclick="pick('easu')">EASU</button>
<span id="label"></span><div id="viewport"><img id="main" src="easu.bmp" alt="EASU"></div>
<h2>Side-by-side (scrollable, not fit-to-window)</h2><div class="row"><div class="pane"><p>Bicubic</p><img id="left" src="bicubic.bmp"></div><div class="pane"><p>EASU</p><img id="right" src="easu.bmp"></div></div>
<p><a href="source.bmp">Original client frame</a> | <a href="legacy_bicubic_sharpen.bmp">Actual legacy bicubic + custom sharpen</a> | <a href="manifest.txt">Parameters / limitations</a></p>
<script>let mode='easu';function pick(m){mode=m;const s=document.getElementById('sharp').value;
document.getElementById('main').src=m+s+'.bmp';document.getElementById('label').textContent=m+s;
document.getElementById('left').src='bicubic'+s+'.bmp';document.getElementById('right').src='easu'+s+'.bmp';}
document.getElementById('sharp').onchange=()=>pick(mode);pick(mode);</script>)HTML";
        html.close(); if (!html) throw std::runtime_error("comparison HTML write failed");
        return true;
    } catch (const std::exception& e) { error = e.what(); return false; }
}
}