#pragma once
#include <onnxruntime_cxx_api.h>
#include <dml_provider_factory.h>
#include <wrl/client.h>
#include <array>
#include <chrono>
#include <exception>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

// Scratch-only, single-threaded, immutable input pair. No session or Run here.
// Caller submits packing on Queue() BEFORE Consume(), with UAV barriers/states,
// and retains command allocators/lists and external producer resources separately.
// A consumer must use the DML1 provider configured on this exact queue/device.
// Its callback may enqueue work but must not retain the borrowed OrtValue pointers.
// Even when the callback throws, a fence is placed after any work it submitted.
// Timeout is NOT cancellation: retain this object and retry Wait(). Destruction
// of an unretired submission fails fast rather than releasing live GPU memory.
class GpuOrtInputs {
    template<class T> using Ptr = Microsoft::WRL::ComPtr<T>;
public:
    enum class State { Ready, Consuming, Pending, Complete, DeviceLost, Unfenced };
    enum class Observation { Pending, Complete, DeviceLost };
    struct Input {
        ID3D12Resource* resource = nullptr;
        // Strong lifetime token for the entire immutable packing job, not just its output.
        std::shared_ptr<void> owner;
    };
    static Observation Classify(UINT64 completed, HRESULT reason) noexcept {
        if (FAILED(reason) || completed == (std::numeric_limits<UINT64>::max)())
            return Observation::DeviceLost;
        return completed >= 1 ? Observation::Complete : Observation::Pending;
    }
    static void Check(HRESULT hr, const char* what) {
        if (FAILED(hr)) throw std::runtime_error(std::string(what) + " HRESULT=" +
            std::to_string(static_cast<unsigned long>(hr)));
    }
    static bool SameObject(IUnknown* a, IUnknown* b) {
        if (!a || !b) return false;
        Ptr<IUnknown> ia, ib;
        Check(a->QueryInterface(IID_PPV_ARGS(&ia)), "first COM identity");
        Check(b->QueryInterface(IID_PPV_ARGS(&ib)), "second COM identity");
        return ia.Get() == ib.Get();
    }
    GpuOrtInputs(const OrtDmlApi* api, ID3D12Device* device, ID3D12CommandQueue* queue,
        std::array<Input, 2> inputs, int64_t width, int64_t height)
        : device_(device), queue_(queue) {
        if (!api || !api->CreateGPUAllocationFromD3DResource || !api->FreeGPUAllocation ||
            !device || !queue) throw std::invalid_argument("missing DML interop/device/queue");
        if (width <= 0 || height <= 0 || width > 16384 || height > 16384)
            throw std::invalid_argument("invalid RGB NCHW dimensions");
        if (queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT)
            throw std::invalid_argument("packing requires a DIRECT queue");
        Ptr<ID3D12Device> queueDevice;
        Check(queue->GetDevice(IID_PPV_ARGS(&queueDevice)), "queue device");
        if (!SameObject(device, queueDevice.Get())) throw std::invalid_argument("queue device mismatch");
        const UINT64 bytes = static_cast<UINT64>(width) * static_cast<UINT64>(height) * 12;
        // Validate both inputs before creating either provider wrapper.
        for (const auto& input : inputs) {
            if (!input.resource || !input.owner) throw std::invalid_argument("missing input/retention token");
            Ptr<ID3D12Device> owner;
            Check(input.resource->GetDevice(IID_PPV_ARGS(&owner)), "resource device");
            if (!SameObject(owner.Get(), device)) throw std::invalid_argument("input device mismatch");
            const auto d = input.resource->GetDesc();
            D3D12_HEAP_PROPERTIES heap{}; D3D12_HEAP_FLAGS flags{};
            Check(input.resource->GetHeapProperties(&heap, &flags), "input heap");
            if (d.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER || d.Width != bytes ||
                d.Format != DXGI_FORMAT_UNKNOWN || d.Layout != D3D12_TEXTURE_LAYOUT_ROW_MAJOR ||
                !(d.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) ||
                heap.Type != D3D12_HEAP_TYPE_DEFAULT)
                throw std::invalid_argument("expected exact-size DEFAULT UAV tensor buffer");
        }
        if (SameObject(inputs[0].resource, inputs[1].resource))
            throw std::invalid_argument("aliased immutable inputs are not supported in this prototype");
        Check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_)), "input retirement fence");
        const std::array<int64_t, 4> shape{1, 3, height, width};
        Ort::MemoryInfo memory("DML", OrtDeviceAllocator, 0, OrtMemTypeDefault);
        for (size_t i = 0; i < tensors_.size(); ++i) {
            auto& t = tensors_[i];
            t.api = api;
            t.owner = std::move(inputs[i].owner);
            t.resource = inputs[i].resource;
            Ort::ThrowOnError(api->CreateGPUAllocationFromD3DResource(t.resource.Get(), &t.allocation));
            t.value = Ort::Value::CreateTensor(memory, t.allocation, static_cast<size_t>(bytes),
                shape.data(), shape.size(), ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT);
        }
    }
    ~GpuOrtInputs() noexcept {
        // Never hide a timeout with an infinite destructor wait or free in-flight buffers.
        // The owning worker must retain this object until Wait() succeeds/device is lost.
        if (state_ == State::Pending || state_ == State::Unfenced) Poll();
        if (state_ == State::Pending || state_ == State::Consuming || state_ == State::Unfenced)
            std::terminate();
    }
    GpuOrtInputs(const GpuOrtInputs&) = delete;
    GpuOrtInputs& operator=(const GpuOrtInputs&) = delete;
    ID3D12CommandQueue* Queue() const noexcept { return queue_.Get(); }
    State GetState() const noexcept { return state_; }
    static constexpr std::array<const char*, 2> Names{"img0", "img1"};

    // Must be invoked before creating a future session. dml/module/session lifetime
    // is caller-owned. This method deliberately does not load DLLs or select adapters.
    void AppendProvider(const OrtDmlApi* api, Ort::SessionOptions& options, IDMLDevice* dml) {
        if (state_ != State::Ready || !api || !api->SessionOptionsAppendExecutionProvider_DML1 || !dml)
            throw std::invalid_argument("provider setup unavailable");
        Ptr<ID3D12Device> owner;
        Check(dml->GetParentDevice(IID_PPV_ARGS(&owner)), "DML parent device");
        if (!SameObject(device_.Get(), owner.Get())) throw std::invalid_argument("DML device mismatch");
        options.DisableMemPattern();
        options.SetExecutionMode(ExecutionMode::ORT_SEQUENTIAL);
        options.AddConfigEntry("session.disable_cpu_ep_fallback", "1");
        Ort::ThrowOnError(api->SessionOptionsAppendExecutionProvider_DML1(options, dml, queue_.Get()));
    }
    using Signal = HRESULT (*)(ID3D12CommandQueue*, ID3D12Fence*, UINT64);
    static HRESULT QueueSignal(ID3D12CommandQueue* queue, ID3D12Fence* fence, UINT64 value) {
        return queue->Signal(fence, value);
    }
    // Signal injection exists for deterministic failure tests; normal calls always
    // use QueueSignal. Retry on the exact same queue cannot retire earlier work early.
    void RetryRetirement(Signal signal = QueueSignal) {
        if (state_ != State::Unfenced || !signal) throw std::logic_error("retirement retry unavailable");
        const HRESULT hr = signal(queue_.Get(), fence_.Get(), 1);
        if (SUCCEEDED(hr)) state_ = State::Pending;
        else if (FAILED(device_->GetDeviceRemovedReason())) state_ = State::DeviceLost;
        Check(hr, "signal input retirement");
    }
    template<class Consumer> void Consume(Consumer&& consumer, Signal signal = QueueSignal) {
        if (state_ != State::Ready) throw std::logic_error("immutable input pair already consumed");
        if (!signal) throw std::invalid_argument("missing retirement signal");
        state_ = State::Consuming;
        const std::array<const OrtValue*, 2> values{tensors_[0].value, tensors_[1].value};
        std::exception_ptr error;
        try { consumer(Names, values, queue_.Get()); } catch (...) { error = std::current_exception(); }
        // Must follow the callback even on an exception: it may have submitted work.
        state_ = State::Unfenced;
        RetryRetirement(signal);
        if (error) std::rethrow_exception(error);
    }
    Observation Poll() noexcept {
        if (state_ == State::Complete) return Observation::Complete;
        if (state_ == State::DeviceLost) return Observation::DeviceLost;
        if (state_ != State::Pending && state_ != State::Unfenced) return Observation::Pending;
        const auto result = Classify(fence_->GetCompletedValue(), device_->GetDeviceRemovedReason());
        if (result == Observation::DeviceLost) state_ = State::DeviceLost;
        else if (result == Observation::Complete && state_ == State::Pending) state_ = State::Complete;
        return state_ == State::Complete ? Observation::Complete :
            state_ == State::DeviceLost ? Observation::DeviceLost : Observation::Pending;
    }
    bool Wait(DWORD timeoutMs) {
        if (timeoutMs == INFINITE) throw std::invalid_argument("bounded waits only");
        if (state_ == State::Ready || state_ == State::Consuming)
            throw std::logic_error("no completed consumer submission to wait for");
        const auto start = std::chrono::steady_clock::now();
        for (;;) {
            const auto status = Poll();
            if (status == Observation::DeviceLost) throw std::runtime_error("input device lost");
            if (status == Observation::Complete) return true;
            if (std::chrono::steady_clock::now() - start >= std::chrono::milliseconds(timeoutMs)) return false;
            // Prototype only: no event lifetime/registration on timed-out waits.
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
private:
    struct Tensor {
        const OrtDmlApi* api = nullptr; // provider table/module must outlive this object
        std::shared_ptr<void> owner;
        Ptr<ID3D12Resource> resource;
        void* allocation = nullptr;
        Ort::Value value{nullptr};
        ~Tensor() {
            value = Ort::Value{nullptr}; // value BEFORE provider wrapper BEFORE resource/owner
            if (allocation) {
                OrtStatus* status = api->FreeGPUAllocation(allocation);
                if (status) Ort::GetApi().ReleaseStatus(status);
            }
        }
    };
    Ptr<ID3D12Device> device_;
    Ptr<ID3D12CommandQueue> queue_;
    Ptr<ID3D12Fence> fence_;
    std::array<Tensor, 2> tensors_;
    State state_ = State::Ready;
};