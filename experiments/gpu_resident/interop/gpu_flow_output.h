#pragma once
#include "gpu_submission_owner.h"
#include "baseline/gpu_input_pack.h"

// Scratch-only fixed-shape output ownership prototype. No model or Run API.
// Single worker, one submission, no reuse. Callback borrows the tensor/resource
// only for enqueueing work on the supplied queue. It must not retain either.
// All external lists/allocators/inputs/session must be held by dependencies.
// A future inference integration must validate session identity and model shape.
class GpuFlowOutput {
    template<class T> using Ptr = Microsoft::WRL::ComPtr<T>;
public:
    using State = GpuOrtInputs::State;
    using Observation = GpuOrtInputs::Observation;
    using Signal = GpuOrtInputs::Signal;
    inline static constexpr std::array<int64_t, 4> Shape{1, 5, 320, 576};
    inline static constexpr UINT64 Bytes = 5ull * 320 * 576 * sizeof(float);

    explicit GpuFlowOutput(std::shared_ptr<GpuProviderContext> context) : context_(std::move(context)) {
        if (!context_) throw std::invalid_argument("output requires provider context");
        tensor_.api = context_->Api();
        tensor_.resource = GpuInputPack::Buffer(context_->Device(), Bytes,
            D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        GpuOrtInputs::Check(context_->Device()->CreateFence(0, D3D12_FENCE_FLAG_NONE,
            IID_PPV_ARGS(&fence_)), "output fence");
        Ort::ThrowOnError(tensor_.api->CreateGPUAllocationFromD3DResource(tensor_.resource.Get(), &tensor_.allocation));
        Ort::MemoryInfo memory("DML", OrtDeviceAllocator, 0, OrtMemTypeDefault);
        tensor_.value = Ort::Value::CreateTensor(memory, tensor_.allocation, static_cast<size_t>(Bytes),
            Shape.data(), Shape.size(), ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT);
    }
    GpuFlowOutput(const GpuFlowOutput&) = delete;
    GpuFlowOutput& operator=(const GpuFlowOutput&) = delete;
    GpuFlowOutput(GpuFlowOutput&&) = delete;
    GpuFlowOutput& operator=(GpuFlowOutput&&) = delete;
    ~GpuFlowOutput() {
        if (state_ == State::Pending || state_ == State::Unfenced) Poll();
        if (state_ != State::Ready && state_ != State::Complete && state_ != State::DeviceLost)
            std::terminate(); // a timeout is not cancellation
    }
    State GetState() const noexcept { return state_; }
    bool BelongsTo(const GpuProviderContext* context) const noexcept { return context_.get() == context; }
    template<class Consumer> void Submit(std::shared_ptr<void> dependencies, Consumer&& consumer,
        Signal signal = GpuOrtInputs::QueueSignal) {
        if (state_ != State::Ready) throw std::logic_error("output already submitted");
        if (!dependencies || !signal) throw std::invalid_argument("output requires dependencies and signal");
        dependencies_ = std::move(dependencies); // BEFORE any callback or queue work
        state_ = State::Consuming;
        std::exception_ptr error;
        try { consumer(static_cast<OrtValue*>(tensor_.value), tensor_.resource.Get(), context_->Queue()); }
        catch (...) { error = std::current_exception(); }
        state_ = State::Unfenced;
        RetrySignal(signal); // follows all callback work, even if the callback threw
        if (error) std::rethrow_exception(error);
    }
    void RetrySignal(Signal signal = GpuOrtInputs::QueueSignal) {
        if (state_ != State::Unfenced || !signal) throw std::logic_error("output signal retry unavailable");
        const HRESULT hr = signal(context_->Queue(), fence_.Get(), 1);
        if (SUCCEEDED(hr)) state_ = State::Pending;
        else if (FAILED(context_->Device()->GetDeviceRemovedReason())) state_ = State::DeviceLost;
        GpuOrtInputs::Check(hr, "output retirement signal");
    }
    Observation Poll() noexcept {
        if (state_ == State::Complete) return Observation::Complete;
        if (state_ == State::DeviceLost) return Observation::DeviceLost;
        if (state_ != State::Pending && state_ != State::Unfenced) return Observation::Pending;
        const auto status = GpuOrtInputs::Classify(fence_->GetCompletedValue(), context_->Device()->GetDeviceRemovedReason());
        if (status == Observation::DeviceLost) state_ = State::DeviceLost;
        else if (status == Observation::Complete && state_ == State::Pending) state_ = State::Complete;
        return state_ == State::Complete ? Observation::Complete :
            state_ == State::DeviceLost ? Observation::DeviceLost : Observation::Pending;
    }
    bool Wait(DWORD timeoutMs) {
        if (timeoutMs == INFINITE) throw std::invalid_argument("bounded output waits only");
        if (state_ == State::Ready || state_ == State::Consuming) throw std::logic_error("output not submitted");
        const auto start = std::chrono::steady_clock::now();
        for (;;) {
            const auto status = Poll();
            if (status == Observation::DeviceLost) throw std::runtime_error("output device lost");
            if (status == Observation::Complete) return true;
            if (std::chrono::steady_clock::now() - start >= std::chrono::milliseconds(timeoutMs)) return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
private:
    struct Tensor {
        const OrtDmlApi* api = nullptr;
        Ptr<ID3D12Resource> resource;
        void* allocation = nullptr;
        Ort::Value value{nullptr};
        ~Tensor() {
            value = Ort::Value{nullptr};
            if (allocation) {
                OrtStatus* status = api->FreeGPUAllocation(allocation);
                if (status) Ort::GetApi().ReleaseStatus(status);
            }
        }
    };
    // Reverse destruction: OrtValue -> wrapper -> buffer -> dependencies -> context.
    std::shared_ptr<GpuProviderContext> context_;
    std::shared_ptr<void> dependencies_;
    Ptr<ID3D12Fence> fence_;
    Tensor tensor_;
    State state_ = State::Ready;
};