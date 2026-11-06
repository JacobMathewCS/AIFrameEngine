#pragma once
#include "gpu_joined_packing.h"
#include "baseline/rife_preprocessor.h"
#include <d3d11_4.h>
#include <dxgi1_4.h>

// Scratch, exclusive single-threaded immediate-context access. Immutable owned
// captures must already be ready on this D3D11 device. No CPU pixel transfer here.
// All construction/recording finishes before any producer commands are executed.
class GpuProducerBridge {
    template<class T> using Ptr = Microsoft::WRL::ComPtr<T>;
    struct Handle {
        HANDLE value = nullptr;
        ~Handle() { if (value) CloseHandle(value); }
        Handle() = default;
        Handle(const Handle&) = delete;
        Handle& operator=(const Handle&) = delete;
    };
public:
    struct Capture {
        ID3D11ShaderResourceView* view = nullptr;
        std::shared_ptr<void> owner;
    };
    enum class Stage { Ready, ProducerUnfenced, ProducerSignaled, ConsumerSubmitted, Retired };
    using ProducerSignal = HRESULT(*)(ID3D11DeviceContext4*, ID3D11Fence*, UINT64);
    using QueueWait = HRESULT(*)(ID3D12CommandQueue*, ID3D12Fence*, UINT64);
    static HRESULT SignalProducer(ID3D11DeviceContext4* c, ID3D11Fence* f, UINT64 v) { return c->Signal(f, v); }
    static HRESULT WaitProducer(ID3D12CommandQueue* q, ID3D12Fence* f, UINT64 v) { return q->Wait(f, v); }
private:
    struct Producer {
        std::array<Capture, 2> captures;
        std::array<Ptr<ID3D11ShaderResourceView>, 2> views;
        Ptr<ID3D11Device> device;
        Ptr<ID3D11DeviceContext4> immediate;
        Ptr<ID3D11DeviceContext> deferred;
        std::array<RifePreprocessor, 2> resize;
        std::array<Ptr<ID3D11Texture2D>, 2> shared;
        std::array<Ptr<ID3D11Texture2D>, 2> originals;
        std::array<Ptr<ID3D11ShaderResourceView>, 2> originalViews;
        std::array<Ptr<ID3D12Resource>, 2> original12;
        std::array<Ptr<ID3D12Resource>, 2> opened;
        Ptr<ID3D11Fence> fence11;
        Ptr<ID3D12Fence> fence12;
        Ptr<ID3D11CommandList> commands;
        Producer(ID3D12Device* consumer, ID3D11DeviceContext* context,
            std::array<Capture, 2> in, UINT width, UINT height) : captures(std::move(in)) {
            if (!context || context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE ||
                !width || !height || width > 16384 || height > 16384)
                throw std::invalid_argument("bridge requires immediate context and bounded dimensions");
            context->GetDevice(&device);
            GpuOrtInputs::Check(context->QueryInterface(IID_PPV_ARGS(&immediate)), "context4");
            Ptr<IDXGIDevice> dxgi; Ptr<IDXGIAdapter> adapter; DXGI_ADAPTER_DESC adapterDesc{};
            GpuOrtInputs::Check(device.As(&dxgi), "producer DXGI device");
            GpuOrtInputs::Check(dxgi->GetAdapter(&adapter), "producer adapter");
            GpuOrtInputs::Check(adapter->GetDesc(&adapterDesc), "producer adapter desc");
            const auto luid = consumer->GetAdapterLuid();
            if (luid.HighPart != adapterDesc.AdapterLuid.HighPart || luid.LowPart != adapterDesc.AdapterLuid.LowPart)
                throw std::invalid_argument("producer/consumer adapter mismatch");
            for (size_t n = 0; n < 2; ++n) {
                if (!captures[n].view || !captures[n].owner) throw std::invalid_argument("capture view/owner missing");
                views[n] = captures[n].view;
                Ptr<ID3D11Device> viewDevice; views[n]->GetDevice(&viewDevice);
                if (!GpuOrtInputs::SameObject(device.Get(), viewDevice.Get())) throw std::invalid_argument("capture device mismatch");
                D3D11_SHADER_RESOURCE_VIEW_DESC sd{}; views[n]->GetDesc(&sd);
                Ptr<ID3D11Resource> resource; Ptr<ID3D11Texture2D> texture;
                views[n]->GetResource(&resource); GpuOrtInputs::Check(resource.As(&texture), "capture texture");
                D3D11_TEXTURE2D_DESC td{}; texture->GetDesc(&td);
                if (sd.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2D || sd.Format != DXGI_FORMAT_B8G8R8A8_UNORM ||
                    sd.Texture2D.MostDetailedMip != 0 || sd.Texture2D.MipLevels != 1 ||
                    td.Format != DXGI_FORMAT_B8G8R8A8_UNORM || td.ArraySize != 1 || td.MipLevels != 1 || td.SampleDesc.Count != 1)
                    throw std::invalid_argument("capture must be single BGRA8 UNORM texture view");
            }
            GpuOrtInputs::Check(device->CreateDeferredContext(0, &deferred), "producer deferred recording");
            Ptr<ID3D11Device5> device5; GpuOrtInputs::Check(device.As(&device5), "producer device5");
            GpuOrtInputs::Check(device5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&fence11)), "producer shared fence");
            Handle fh;
            GpuOrtInputs::Check(fence11->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &fh.value), "fence handle");
            GpuOrtInputs::Check(consumer->OpenSharedHandle(fh.value, IID_PPV_ARGS(&fence12)), "open producer fence");
            for (size_t n = 0; n < 2; ++n) {
                std::string error;
                if (!resize[n].Initialize(device.Get(), width, height, error)) throw std::runtime_error(error);
                D3D11_TEXTURE2D_DESC td{}; td.Width = width; td.Height = height;
                td.ArraySize = td.MipLevels = td.SampleDesc.Count = 1;
                td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.Usage = D3D11_USAGE_DEFAULT;
                td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                td.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
                GpuOrtInputs::Check(device->CreateTexture2D(&td, nullptr, &shared[n]), "bridge shared texture");
                Ptr<IDXGIResource1> resource; GpuOrtInputs::Check(shared[n].As(&resource), "shared DXGI resource");
                Handle th;
                GpuOrtInputs::Check(resource->CreateSharedHandle(nullptr,
                    DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &th.value), "texture handle");
                GpuOrtInputs::Check(consumer->OpenSharedHandle(th.value, IID_PPV_ARGS(&opened[n])), "open shared texture");
                // Snapshot the original once. Both resize and reconstruction use this
                // exact owned image; callers cannot supply an unrelated reconstruction pair.
                Ptr<ID3D11Resource> captureResource; views[n]->GetResource(&captureResource);
                Ptr<ID3D11Texture2D> captureTexture;
                GpuOrtInputs::Check(captureResource.As(&captureTexture), "original texture");
                D3D11_TEXTURE2D_DESC originalDesc{}; captureTexture->GetDesc(&originalDesc);
                originalDesc.Usage = D3D11_USAGE_DEFAULT; originalDesc.CPUAccessFlags = 0;
                originalDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                originalDesc.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
                GpuOrtInputs::Check(device->CreateTexture2D(&originalDesc, nullptr, &originals[n]), "original snapshot");
                GpuOrtInputs::Check(device->CreateShaderResourceView(originals[n].Get(), nullptr, &originalViews[n]), "original view");
                Ptr<IDXGIResource1> originalResource;
                GpuOrtInputs::Check(originals[n].As(&originalResource), "original DXGI");
                Handle originalHandle;
                GpuOrtInputs::Check(originalResource->CreateSharedHandle(nullptr,
                    DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &originalHandle.value), "original handle");
                GpuOrtInputs::Check(consumer->OpenSharedHandle(originalHandle.value, IID_PPV_ARGS(&original12[n])), "original open");
                deferred->CopyResource(originals[n].Get(), captureResource.Get());
                if (!resize[n].Resize(deferred.Get(), originalViews[n].Get())) throw std::runtime_error("record baseline resize");
                Ptr<ID3D11Resource> resized; resize[n].OutputView()->GetResource(&resized);
                deferred->CopyResource(shared[n].Get(), resized.Get());
            }
            GpuOrtInputs::Check(deferred->FinishCommandList(FALSE, &commands), "finish producer recording");
        }
    };
public:
    GpuProducerBridge(std::shared_ptr<GpuProviderContext> context, ID3D11DeviceContext* producerContext,
        std::array<Capture, 2> captures, UINT width, UINT height,
        std::shared_ptr<GpuBoundSession> session = {}) : context_(std::move(context)), session_(session), width_(width), height_(height) {
        if (!context_) throw std::invalid_argument("missing provider");
        if (session && !session->BelongsTo(context_.get())) throw std::invalid_argument("bridge session mismatch");
        producer_ = std::make_shared<Producer>(context_->Device(), producerContext, std::move(captures), width, height);
        std::array<GpuJoinedPacking::Source, 2> sources{{
            {producer_->opened[0].Get(), producer_}, {producer_->opened[1].Get(), producer_}}};
        joined_ = std::make_unique<GpuJoinedPacking>(context_, sources, std::move(session));
        // joined owns the producer token before any immediate-context execution.
    }
    GpuProducerBridge(const GpuProducerBridge&) = delete;
    GpuProducerBridge& operator=(const GpuProducerBridge&) = delete;
    ~GpuProducerBridge() {
        // A D3D12-only fence cannot justify releasing an unfenced D3D11 producer.
        if (stage_ != Stage::Ready && stage_ != Stage::Retired) std::terminate();
    }
    void SubmitProducer(ProducerSignal signal = SignalProducer) {
        if (stage_ != Stage::Ready || !signal) throw std::logic_error("producer submission unavailable");
        stage_ = Stage::ProducerUnfenced;
        producer_->immediate->ExecuteCommandList(producer_->commands.Get(), FALSE);
        RetryProducerSignal(signal);
    }
    void RetryProducerSignal(ProducerSignal signal = SignalProducer) {
        if (stage_ != Stage::ProducerUnfenced || !signal) throw std::logic_error("producer signal retry unavailable");
        const HRESULT hr = signal(producer_->immediate.Get(), producer_->fence11.Get(), 1);
        producer_->immediate->Flush();
        if (SUCCEEDED(hr)) stage_ = Stage::ProducerSignaled;
        GpuOrtInputs::Check(hr, "producer signal");
    }
    template<class Consumer> void SubmitConsumer(Consumer&& consume,
        GpuOrtInputs::Signal signal = GpuOrtInputs::QueueSignal, QueueWait wait = WaitProducer) {
        if (stage_ != Stage::ProducerSignaled || !signal || !wait) throw std::logic_error("consumer submission unavailable");
        // Failed wait leaves producer ownership intact and packing unsubmitted.
        GpuOrtInputs::Check(wait(context_->Queue(), producer_->fence12.Get(), 1), "producer GPU wait");
        stage_ = Stage::ConsumerSubmitted;
        joined_->Submit(std::forward<Consumer>(consume), signal);
    }
    void RetryRetirement(GpuOrtInputs::Signal signal = GpuOrtInputs::QueueSignal) {
        if (stage_ != Stage::ConsumerSubmitted) throw std::logic_error("no consumer to fence");
        joined_->RetrySignal(signal);
    }
    GpuSubmissionOwner::Retirement Retire(DWORD ms) {
        using R = GpuSubmissionOwner::Retirement;
        if (ms == INFINITE) throw std::invalid_argument("bounded retirement only");
        if (stage_ == Stage::Ready) throw std::logic_error("bridge not submitted");
        if (stage_ == Stage::Retired) return R::Empty;
        // No cancellation: explicit producer signal/wait/consumer retry is required.
        if (stage_ != Stage::ConsumerSubmitted) return R::Pending;
        const auto result = joined_->Retire(ms);
        if (result == R::Complete) {
            // Consumer fence follows its GPU wait; verify the producer too before release.
            GpuOrtInputs::Check(producer_->device->GetDeviceRemovedReason(), "producer device lost");
            const UINT64 done = producer_->fence11->GetCompletedValue();
            if (done == UINT64_MAX || done < 1) throw std::runtime_error("producer completion not established");
            joined_.reset(); producer_.reset(); context_.reset(); stage_ = Stage::Retired;
        }
        // Device loss on one API does not prove completion on the other. Retain and
        // fail fast at destruction; cross-device-loss shutdown is intentionally absent.
        return result;
    }
    ID3D12Resource* Original(size_t n) const {
        if (!producer_ || n >= 2) throw std::invalid_argument("original unavailable");
        return producer_->original12[n].Get();
    }
    Stage GetStage() const noexcept { return stage_; }
    bool BelongsTo(const GpuProviderContext* context) const noexcept { return context_.get() == context; }
    bool UsesSession(const GpuBoundSession* session) const noexcept { return session_.get() == session; }
    bool HasFlowShape() const noexcept { return width_ == 576 && height_ == 320; }
    GpuOrtInputs::State ConsumerState() const {
        if (!joined_) throw std::logic_error("bridge retired");
        return joined_->State();
    }
    ID3D12Resource* Output(size_t n) const {
        if (!joined_) throw std::logic_error("bridge retired");
        return joined_->Output(n);
    }
    std::weak_ptr<void> ProducerLifetime() const noexcept { return producer_; }
    std::weak_ptr<void> PackingLifetime() const noexcept { return joined_ ? joined_->Lifetime() : std::weak_ptr<void>{}; }
    UINT64 ProducerCompleted() const {
        if (!producer_) throw std::logic_error("bridge retired");
        return producer_->fence11->GetCompletedValue();
    }
    UINT64 CpuReadbacks() const {
        if (!producer_) throw std::logic_error("bridge retired");
        return producer_->resize[0].Readbacks() + producer_->resize[1].Readbacks();
    }
private:
    std::shared_ptr<GpuProviderContext> context_;
    std::shared_ptr<GpuBoundSession> session_;
    UINT width_ = 0, height_ = 0;
    std::shared_ptr<Producer> producer_;
    std::unique_ptr<GpuJoinedPacking> joined_;
    Stage stage_ = Stage::Ready;
};