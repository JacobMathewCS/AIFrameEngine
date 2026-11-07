#pragma once
#include "plain_texture.h"
#include <d3d11_4.h>
#include <dxgi1_4.h>

// Scratch single-use, same-adapter output sharing. No pixel copy or CPU mapping.
// The entire object AND callback resources must be held by the outer retirement
// owner (GpuFlowOutput dependencies / GpuResidentRun hookResources). D3D11 immediate
// context access must be exclusive. Borrowed views must not escape the callback.
// Signal/wait failure is fail-fast at destruction unless completion is established;
// device-loss recovery is deliberately not implemented.
class TextureHandoff {
    template<class T> using Ptr = Microsoft::WRL::ComPtr<T>;
    struct Handle {
        HANDLE value = nullptr;
        ~Handle() { if (value) CloseHandle(value); }
    };
    static void Check(HRESULT hr, const char* text) { GpuOrtInputs::Check(hr, text); }
public:
    TextureHandoff(std::shared_ptr<GpuProviderContext> context,
        std::shared_ptr<PlainTexture> reconstruction, ID3D11DeviceContext* immediate)
        : context_(std::move(context)), reconstruction_(std::move(reconstruction)) {
        if (!context_ || !reconstruction_ || !immediate || immediate->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE)
            throw std::invalid_argument("handoff requires context, reconstruction and immediate consumer");
        Ptr<ID3D12Device> owner;
        Check(reconstruction_->Output()->GetDevice(IID_PPV_ARGS(&owner)), "handoff output device");
        if (!GpuOrtInputs::SameObject(owner.Get(), context_->Device()))
            throw std::invalid_argument("handoff output device mismatch");
        Ptr<ID3D11Device> device; immediate->GetDevice(&device);
        Check(device.As(&device_), "handoff device5");
        Check(immediate->QueryInterface(IID_PPV_ARGS(&immediate_)), "handoff context4");
        Ptr<IDXGIDevice> dxgi; Ptr<IDXGIAdapter> adapter; DXGI_ADAPTER_DESC desc{};
        Check(device.As(&dxgi), "handoff DXGI device"); Check(dxgi->GetAdapter(&adapter), "handoff adapter");
        Check(adapter->GetDesc(&desc), "handoff adapter desc");
        const auto luid = context_->Device()->GetAdapterLuid();
        if (desc.AdapterLuid.HighPart != luid.HighPart || desc.AdapterLuid.LowPart != luid.LowPart)
            throw std::invalid_argument("handoff adapter mismatch");
        D3D12_HEAP_PROPERTIES heap{}; D3D12_HEAP_FLAGS flags{};
        Check(reconstruction_->Output()->GetHeapProperties(&heap, &flags), "handoff output heap");
        if (!(flags & D3D12_HEAP_FLAG_SHARED)) throw std::invalid_argument("handoff output must be shared");
        Handle textureHandle, fenceHandle;
        Check(context_->Device()->CreateSharedHandle(reconstruction_->Output(), nullptr, GENERIC_ALL,
            nullptr, &textureHandle.value), "handoff texture handle");
        Check(device_->OpenSharedResource1(textureHandle.value, IID_PPV_ARGS(&texture_)), "handoff open texture");
        Check(device_->CreateShaderResourceView(texture_.Get(), nullptr, &view_), "handoff view");
        Check(context_->Device()->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&fence12_)), "handoff fence");
        Check(context_->Device()->CreateSharedHandle(fence12_.Get(), nullptr, GENERIC_ALL,
            nullptr, &fenceHandle.value), "handoff fence handle");
        Check(device_->OpenSharedFence(fenceHandle.value, IID_PPV_ARGS(&fence11_)), "handoff open fence");
    }
    TextureHandoff(const TextureHandoff&) = delete;
    TextureHandoff& operator=(const TextureHandoff&) = delete;
    TextureHandoff(TextureHandoff&&) = delete;
    TextureHandoff& operator=(TextureHandoff&&) = delete;
    ~TextureHandoff() {
        // Merely retaining the texture does not retain GPU command resources.
        if (submitted_ && (!joined_ || fence12_->GetCompletedValue() < 2 ||
            FAILED(context_->Device()->GetDeviceRemovedReason()) || FAILED(device_->GetDeviceRemovedReason())))
            std::terminate();
    }
    template<class Consumer> void Submit(ID3D12Resource* flow, ID3D12CommandQueue* queue, Consumer&& consume) {
        if (submitted_) throw std::logic_error("handoff already submitted");
        if (!queue || !GpuOrtInputs::SameObject(queue, context_->Queue()))
            throw std::invalid_argument("handoff provider queue mismatch");
        reconstruction_->Submit(flow, queue);
        submitted_ = true;
        Check(queue->Signal(fence12_.Get(), 1), "handoff ready signal");
        Check(immediate_->Wait(fence11_.Get(), 1), "handoff consumer wait");
        std::exception_ptr error;
        try { consume(immediate_.Get(), view_.Get()); }
        catch (...) { error = std::current_exception(); }
        // Fence callback work even when it throws; no callback/model retry.
        Check(immediate_->Signal(fence11_.Get(), 2), "handoff consumer signal");
        immediate_->Flush();
        Check(queue->Wait(fence12_.Get(), 2), "handoff return wait");
        joined_ = true;
        if (error) std::rethrow_exception(error);
    }
private:
    std::shared_ptr<GpuProviderContext> context_;
    std::shared_ptr<PlainTexture> reconstruction_;
    Ptr<ID3D11Device5> device_;
    Ptr<ID3D11DeviceContext4> immediate_;
    Ptr<ID3D11Texture2D> texture_;
    Ptr<ID3D11ShaderResourceView> view_;
    Ptr<ID3D12Fence> fence12_;
    Ptr<ID3D11Fence> fence11_;
    bool submitted_ = false, joined_ = false;
};