#pragma once
#include "gpu_ort_inputs.h"
#include <utility>

class GpuBoundSession;
class GpuResidentRun;

// Scratch provider context: borrows no external DLL/device/queue lifetime.
// Model loading is restricted to GpuBoundSession. Options are private/configured once.
class GpuProviderContext {
    template<class T> using Ptr = Microsoft::WRL::ComPtr<T>;
    struct Module {
        HMODULE value = nullptr;
        ~Module() { if (value) FreeLibrary(value); }
    };
public:
    GpuProviderContext(ID3D12Device* device, ID3D12CommandQueue* queue)
        : device_(device), queue_(queue) {
        if (!device || !queue || queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT)
            throw std::invalid_argument("provider requires device and DIRECT queue");
        Ptr<ID3D12Device> owner;
        GpuOrtInputs::Check(queue->GetDevice(IID_PPV_ARGS(&owner)), "provider queue device");
        if (!GpuOrtInputs::SameObject(device, owner.Get())) throw std::invalid_argument("provider queue mismatch");
        wchar_t path[32768]{};
        const DWORD size = GetModuleFileNameW(nullptr, path, 32768);
        if (!size || size >= 32768) throw std::runtime_error("executable path unavailable");
        std::wstring directory(path, size);
        const auto slash = directory.find_last_of(L"\\/");
        if (slash == std::wstring::npos) throw std::runtime_error("executable directory unavailable");
        directory.resize(slash + 1);
        module_.value = LoadLibraryExW((directory + L"DirectML.dll").c_str(), nullptr,
            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (!module_.value) throw std::runtime_error("local DirectML.dll load failed");
        using Create = HRESULT(WINAPI*)(ID3D12Device*, DML_CREATE_DEVICE_FLAGS, REFIID, void**);
        const auto create = reinterpret_cast<Create>(GetProcAddress(module_.value, "DMLCreateDevice"));
        if (!create) throw std::runtime_error("DMLCreateDevice unavailable");
        GpuOrtInputs::Check(create(device, DML_CREATE_DEVICE_FLAG_NONE, IID_PPV_ARGS(&dml_)), "DML create");
        Ort::ThrowOnError(Ort::GetApi().GetExecutionProviderApi("DML", ORT_API_VERSION,
            reinterpret_cast<const void**>(&api_)));
        if (!api_ || !api_->SessionOptionsAppendExecutionProvider_DML1 ||
            !api_->CreateGPUAllocationFromD3DResource || !api_->FreeGPUAllocation)
            throw std::runtime_error("DML interop unavailable");
        options_.DisableMemPattern();
        options_.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
        options_.SetExecutionMode(ExecutionMode::ORT_SEQUENTIAL);
        options_.AddConfigEntry("session.disable_cpu_ep_fallback", "1");
        Ort::ThrowOnError(api_->SessionOptionsAppendExecutionProvider_DML1(options_, dml_.Get(), queue));
    }
    GpuProviderContext(const GpuProviderContext&) = delete;
    GpuProviderContext& operator=(const GpuProviderContext&) = delete;
    ID3D12Device* Device() const noexcept { return device_.Get(); }
    ID3D12CommandQueue* Queue() const noexcept { return queue_.Get(); }
    IDMLDevice* Dml() const noexcept { return dml_.Get(); }
    const OrtDmlApi* Api() const noexcept { return api_; }
private:
    friend class GpuBoundSession;
    Module module_; // released last
    Ort::Env env_{ORT_LOGGING_LEVEL_WARNING, "ScratchProviderNoInference"};
    Ptr<ID3D12Device> device_;
    Ptr<ID3D12CommandQueue> queue_;
    Ptr<IDMLDevice> dml_;
    const OrtDmlApi* api_ = nullptr;
    Ort::SessionOptions options_;
};

// Factory-only session identity. No raw session/options accessor. The narrow
// parity Run entry is compiled but only invoked by the separately gated harness.
// Model bytes are an immutable caller-pinned snapshot, with no external-data files.
class GpuBoundSession {
public:
    static std::shared_ptr<GpuBoundSession> Create(std::shared_ptr<GpuProviderContext> context,
        const void* modelBytes, size_t modelSize);
    GpuBoundSession(const GpuBoundSession&) = delete;
    GpuBoundSession& operator=(const GpuBoundSession&) = delete;
    bool BelongsTo(const GpuProviderContext* context) const noexcept { return context_.get() == context; }
    void RunForParity(const std::array<const OrtValue*, 2>& inputs, Ort::Value& cpuOutput, bool gpuInputs);
    void ValidateFlowContract();
private:
    friend class GpuResidentRun;
    void RunResident(const std::array<const OrtValue*, 2>& inputs, OrtValue* output);
    GpuBoundSession(std::shared_ptr<GpuProviderContext> context, const void* modelBytes, size_t modelSize);
    std::shared_ptr<GpuProviderContext> context_; // outlives session, including env/DLL
    Ort::Session session_{nullptr};
};

// Single-worker, single-flight owner. Ownership is installed BEFORE callback/work;
// callback exceptions and failed signals leave the slot occupied. No detached work,
// background thread or hidden infinite wait. Timeout requires explicit retry/drain.
class GpuSubmissionOwner {
public:
    GpuSubmissionOwner() = default;
    GpuSubmissionOwner(const GpuSubmissionOwner&) = delete;
    GpuSubmissionOwner& operator=(const GpuSubmissionOwner&) = delete;
    GpuSubmissionOwner(GpuSubmissionOwner&&) = delete;
    GpuSubmissionOwner& operator=(GpuSubmissionOwner&&) = delete;
    enum class Retirement { Empty, Pending, Complete, DeviceLost };
    void Adopt(std::shared_ptr<GpuProviderContext> context,
        std::unique_ptr<GpuOrtInputs>& inputs, std::shared_ptr<void> submissionResources,
        std::shared_ptr<GpuBoundSession> session = {}) {
        if (inputs_) throw std::logic_error("submission slot occupied");
        if (!context || !inputs || !submissionResources || inputs->GetState() != GpuOrtInputs::State::Ready)
            throw std::invalid_argument("incomplete ready submission");
        if (!GpuOrtInputs::SameObject(context->Queue(), inputs->Queue()))
            throw std::invalid_argument("submission/provider queue mismatch");
        if (session && !session->BelongsTo(context.get()))
            throw std::invalid_argument("session/provider context mismatch");
        context_ = std::move(context);
        session_ = std::move(session);
        resources_ = std::move(submissionResources);
        inputs_ = std::move(inputs); // no allocation or GPU work in Adopt
    }
    template<class Consumer> void Submit(Consumer&& consumer,
        GpuOrtInputs::Signal signal = GpuOrtInputs::QueueSignal) {
        if (!inputs_) throw std::logic_error("no adopted submission");
        // Producer submission belongs INSIDE this callback, not before Adopt.
        inputs_->Consume(std::forward<Consumer>(consumer), signal);
    }
    void RetrySignal(GpuOrtInputs::Signal signal = GpuOrtInputs::QueueSignal) {
        if (!inputs_) throw std::logic_error("no adopted submission");
        inputs_->RetryRetirement(signal);
    }
    Retirement Retire(DWORD timeoutMs) {
        if (timeoutMs == INFINITE) throw std::invalid_argument("bounded retirement only");
        if (!inputs_) return Retirement::Empty;
        // Wait throws on device loss; distinguish it from other programming errors.
        bool complete = false;
        try { complete = inputs_->Wait(timeoutMs); }
        catch (...) {
            if (inputs_->GetState() != GpuOrtInputs::State::DeviceLost) throw;
        }
        const bool lost = inputs_->GetState() == GpuOrtInputs::State::DeviceLost;
        if (!complete && !lost) return Retirement::Pending;
        // Wrappers/resources/session disappear before provider context/DLL.
        inputs_.reset(); resources_.reset(); session_.reset(); context_.reset();
        return lost ? Retirement::DeviceLost : Retirement::Complete;
    }
    bool Occupied() const noexcept { return bool(inputs_); }
    GpuOrtInputs::State State() const {
        if (!inputs_) throw std::logic_error("empty owner");
        return inputs_->GetState();
    }
    // The worker MUST outlive pending work. Destroying an occupied pending owner
    // still invokes the adapter fail-fast guard; this is not shutdown recovery.
private:
    std::shared_ptr<GpuProviderContext> context_;
    std::shared_ptr<GpuBoundSession> session_;
    std::shared_ptr<void> resources_;
    std::unique_ptr<GpuOrtInputs> inputs_;
};