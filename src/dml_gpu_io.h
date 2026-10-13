#pragma once
#include <onnxruntime_cxx_api.h>
#include <dml_provider_factory.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <array>
#include <chrono>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

// Stage-one GPU binding bridge. Single worker, one in-flight Run. Upload and
// readback are explicit, NOT zero-copy D3D11 interop. All GPU work uses one DIRECT
// queue and every reused resource is fence-protected. No queue priority changes.
class DmlGpuIo {
    template<class T> using Ptr = Microsoft::WRL::ComPtr<T>;
    using Clock = std::chrono::steady_clock;
    struct Handle {
        HANDLE value = nullptr;
        ~Handle() { if (value) CloseHandle(value); }
    };
    struct Module {
        HMODULE value = nullptr;
        ~Module() { if (value) FreeLibrary(value); }
    };
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
public:
    struct Timing {
        double uploadSubmitMs = 0, runCpuMs = 0, finishWaitMs = 0, readbackCpuMs = 0;
        // Queue elapsed timestamps include scheduling/idle gaps, NOT isolated
        // kernel execution. No subtraction of host time from GPU time is valid.
        double uploadQueueMs = -1, inferenceQueueMs = -1, readbackQueueMs = -1;
    };
    explicit DmlGpuIo(UINT adapterIndex) {
        Ort::ThrowOnError(Ort::GetApi().GetExecutionProviderApi("DML", ORT_API_VERSION,
            reinterpret_cast<const void**>(&api_)));
        Ptr<IDXGIFactory4> factory;
        Check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "DXGI factory");
        Ptr<IDXGIAdapter1> adapter;
        Check(factory->EnumAdapters1(adapterIndex, &adapter), "DML adapter enumeration");
        Check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device_)), "D3D12 device");
        // Load only the DLL deployed beside this executable, not an arbitrary PATH entry.
        wchar_t path[32768] = {};
        DWORD size = GetModuleFileNameW(nullptr, path, 32768);
        if (!size || size >= 32768) throw std::runtime_error("executable path unavailable");
        std::wstring directory(path, size);
        directory.resize(directory.find_last_of(L"\\/") + 1);
        module_.value = LoadLibraryExW((directory + L"DirectML.dll").c_str(), nullptr,
            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (!module_.value) throw std::runtime_error("deployed DirectML.dll could not load");
        using CreateDevice = HRESULT(WINAPI*)(ID3D12Device*, DML_CREATE_DEVICE_FLAGS, REFIID, void**);
        auto create = reinterpret_cast<CreateDevice>(GetProcAddress(module_.value, "DMLCreateDevice"));
        if (!create) throw std::runtime_error("DMLCreateDevice export missing");
        Check(create(device_.Get(), DML_CREATE_DEVICE_FLAG_NONE, IID_PPV_ARGS(&dml_)), "DML device");
        D3D12_COMMAND_QUEUE_DESC desc = {};
        desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        Check(device_->CreateCommandQueue(&desc, IID_PPV_ARGS(&queue_)), "DML queue");
        Check(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_)), "DML fence");
        event_.value = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!event_.value) throw std::runtime_error("DML fence event");
        for (size_t i = 0; i < lists_.size(); ++i) {
            Check(device_->CreateCommandAllocator(desc.Type, IID_PPV_ARGS(&allocators_[i])), "DML command allocator");
            Check(device_->CreateCommandList(0, desc.Type, allocators_[i].Get(), nullptr,
                IID_PPV_ARGS(&lists_[i])), "DML command list");
            Check(lists_[i]->Close(), "close initial DML list");
        }
        D3D12_QUERY_HEAP_DESC query = {};
        query.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
        query.Count = 4;
        Check(device_->CreateQueryHeap(&query, IID_PPV_ARGS(&queries_)), "DML timestamp heap");
        Check(queue_->GetTimestampFrequency(&frequency_), "DML timestamp frequency");
        if (!frequency_) throw std::runtime_error("zero queue timestamp frequency");
        queryReadback_ = Buffer(32, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    }
    ~DmlGpuIo() {
        // Session must be destroyed before this object. Drain queued copies even
        // when an ORT call threw, before releasing resources/allocator wrappers.
        if (queue_ && fence_ && event_.value && SUCCEEDED(device_->GetDeviceRemovedReason())) {
            const UINT64 value = ++fenceValue_;
            if (SUCCEEDED(queue_->Signal(fence_.Get(), value)) && fence_->GetCompletedValue() < value &&
                SUCCEEDED(fence_->SetEventOnCompletion(value, event_.value)))
                WaitForSingleObject(event_.value, INFINITE);
        }
    }
    DmlGpuIo(const DmlGpuIo&) = delete;
    DmlGpuIo& operator=(const DmlGpuIo&) = delete;
    void AppendProvider(Ort::SessionOptions& options) {
        options.DisableMemPattern();
        options.SetExecutionMode(ExecutionMode::ORT_SEQUENTIAL);
        options.AddConfigEntry("session.disable_cpu_ep_fallback", "1");
        Ort::ThrowOnError(api_->SessionOptionsAppendExecutionProvider_DML1(options, dml_.Get(), queue_.Get()));
    }
    void Configure(int64_t width, int64_t height, int64_t outputChannels) {
        if (configured_) throw std::logic_error("GPU IO already configured");
        if (width <= 0 || height <= 0 || width > 16384 || height > 16384 ||
            (outputChannels != 3 && outputChannels != 5)) throw std::invalid_argument("GPU IO shape");
        const size_t plane = static_cast<size_t>(width) * static_cast<size_t>(height);
        inputBytes_ = plane * 3 * sizeof(float);
        outputBytes_ = plane * static_cast<size_t>(outputChannels) * sizeof(float);
        std::array<int64_t, 4> input = {1, 3, height, width}, output = {1, outputChannels, height, width};
        MakeTensor(tensors_[0], input, inputBytes_);
        MakeTensor(tensors_[1], input, inputBytes_);
        MakeTensor(tensors_[2], output, outputBytes_);
        upload_ = Buffer(inputBytes_ * 2, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
        readback_ = Buffer(outputBytes_, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
        output_.resize(outputBytes_ / sizeof(float));
        configured_ = true;
    }
    const std::vector<float>& Run(Ort::Session& session, const Ort::RunOptions& options,
        const std::vector<float>& first, const std::vector<float>& second, const char* outputName) {
        if (!configured_ || poisoned_) throw std::runtime_error("GPU IO unavailable");
        if (first.size() != inputBytes_/sizeof(float) || second.size() != first.size())
            throw std::invalid_argument("GPU IO input size mismatch");
        timing_ = {};
        try {
            auto start = Clock::now();
            void* mapped = nullptr;
            D3D12_RANGE empty = {0, 0};
            Check(upload_->Map(0, &empty, &mapped), "map upload");
            memcpy(mapped, first.data(), inputBytes_);
            memcpy(static_cast<uint8_t*>(mapped) + inputBytes_, second.data(), inputBytes_);
            D3D12_RANGE written = {0, inputBytes_ * 2};
            upload_->Unmap(0, &written);
            BeginList(0);
            auto* list = lists_[0].Get();
            list->EndQuery(queries_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);
            for (int i = 0; i < 2; ++i) {
                Transition(list, tensors_[i].resource.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
                list->CopyBufferRegion(tensors_[i].resource.Get(), 0, upload_.Get(), i * inputBytes_, inputBytes_);
                Transition(list, tensors_[i].resource.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            }
            list->EndQuery(queries_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1);
            Execute(0);
            auto uploaded = Clock::now();
            // Device-resident values; ORT does not perform CPU tensor copies.
            // Separate reference wrappers borrow handles, owned by tensors_.
            const char* names[] = {"img0", "img1"};
            const char* outputs[] = {outputName};
            const OrtValue* inputs[] = {tensors_[0].value, tensors_[1].value};
            OrtValue* outputValue = tensors_[2].value;
            Ort::ThrowOnError(Ort::GetApi().Run(session, options, names, inputs, 2, outputs, 1, &outputValue));
            auto ran = Clock::now();
            BeginList(1);
            list = lists_[1].Get();
            list->EndQuery(queries_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 2);
            Transition(list, tensors_[2].resource.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
            list->CopyBufferRegion(readback_.Get(), 0, tensors_[2].resource.Get(), 0, outputBytes_);
            Transition(list, tensors_[2].resource.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            list->EndQuery(queries_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 3);
            list->ResolveQueryData(queries_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 4, queryReadback_.Get(), 0);
            Execute(1);
            Wait();
            auto finished = Clock::now();
            D3D12_RANGE bytes = {0, outputBytes_};
            Check(readback_->Map(0, &bytes, &mapped), "map output");
            memcpy(output_.data(), mapped, outputBytes_);
            readback_->Unmap(0, &empty);
            D3D12_RANGE queryBytes = {0, 32};
            Check(queryReadback_->Map(0, &queryBytes, &mapped), "map timestamps");
            UINT64 stamps[4];
            memcpy(stamps, mapped, sizeof(stamps));
            queryReadback_->Unmap(0, &empty);
            timing_.uploadSubmitMs = Ms(start, uploaded);
            timing_.runCpuMs = Ms(uploaded, ran);
            timing_.finishWaitMs = Ms(ran, finished);
            timing_.readbackCpuMs = Ms(finished, Clock::now());
            if (stamps[0] <= stamps[1] && stamps[1] <= stamps[2] && stamps[2] <= stamps[3]) {
                timing_.uploadQueueMs = double(stamps[1]-stamps[0])*1000.0/frequency_;
                timing_.inferenceQueueMs = double(stamps[2]-stamps[1])*1000.0/frequency_;
                timing_.readbackQueueMs = double(stamps[3]-stamps[2])*1000.0/frequency_;
            }
            return output_;
        } catch (...) { poisoned_ = true; throw; }
    }
    const Timing& LastTiming() const { return timing_; }
    const std::vector<float>& Output() const { return output_; }
private:
    static void Check(HRESULT hr, const char* stage) {
        if (FAILED(hr)) throw std::runtime_error(std::string(stage) + " HRESULT=" + std::to_string(static_cast<unsigned long>(hr)));
    }
    static double Ms(Clock::time_point a, Clock::time_point b) {
        return std::chrono::duration<double, std::milli>(b-a).count();
    }
    Ptr<ID3D12Resource> Buffer(size_t bytes, D3D12_HEAP_TYPE heap, D3D12_RESOURCE_STATES state) {
        D3D12_HEAP_PROPERTIES properties = {};
        properties.Type = heap;
        properties.CreationNodeMask = properties.VisibleNodeMask = 1;
        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = bytes; desc.Height = desc.DepthOrArraySize = desc.MipLevels = 1;
        desc.SampleDesc.Count = 1; desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (heap == D3D12_HEAP_TYPE_DEFAULT) desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        Ptr<ID3D12Resource> resource;
        Check(device_->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE, &desc, state,
            nullptr, IID_PPV_ARGS(&resource)), "create tensor/copy buffer");
        return resource;
    }
    void MakeTensor(Tensor& tensor, const std::array<int64_t,4>& shape, size_t bytes) {
        tensor.api = api_;
        tensor.resource = Buffer(bytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Ort::ThrowOnError(api_->CreateGPUAllocationFromD3DResource(tensor.resource.Get(), &tensor.allocation));
        // DML EP uses a single device within a session; its memory device id is 0,
        // independently of the physical DXGI adapter index used to create device_.
        Ort::MemoryInfo memory("DML", OrtDeviceAllocator, 0, OrtMemTypeDefault);
        tensor.value = Ort::Value::CreateTensor(memory, tensor.allocation, bytes,
            shape.data(), shape.size(), ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT);
    }
    static void Transition(ID3D12GraphicsCommandList* list, ID3D12Resource* resource,
        D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition = {resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after};
        list->ResourceBarrier(1, &barrier);
    }
    void BeginList(size_t index) {
        Check(allocators_[index]->Reset(), "reset allocator");
        Check(lists_[index]->Reset(allocators_[index].Get(), nullptr), "reset list");
    }
    void Execute(size_t index) {
        Check(lists_[index]->Close(), "close copy list");
        ID3D12CommandList* lists[] = {lists_[index].Get()};
        queue_->ExecuteCommandLists(1, lists);
    }
    void Wait() {
        const UINT64 value = ++fenceValue_;
        Check(queue_->Signal(fence_.Get(), value), "signal GPU IO fence");
        if (fence_->GetCompletedValue() < value) {
            Check(fence_->SetEventOnCompletion(value, event_.value), "set GPU IO fence event");
            if (WaitForSingleObject(event_.value, 30000) != WAIT_OBJECT_0)
                throw std::runtime_error("GPU IO fence wait failed/timed out");
        }
        Check(device_->GetDeviceRemovedReason(), "GPU IO device status");
    }
    // DLL declared first, destroyed last after DML objects and wrappers.
    Module module_;
    const OrtDmlApi* api_ = nullptr;
    Ptr<ID3D12Device> device_;
    Ptr<IDMLDevice> dml_;
    Ptr<ID3D12CommandQueue> queue_;
    Ptr<ID3D12Fence> fence_;
    Handle event_;
    std::array<Ptr<ID3D12CommandAllocator>,2> allocators_;
    std::array<Ptr<ID3D12GraphicsCommandList>,2> lists_;
    Ptr<ID3D12QueryHeap> queries_;
    Ptr<ID3D12Resource> upload_, readback_, queryReadback_;
    std::array<Tensor,3> tensors_;
    std::vector<float> output_;
    size_t inputBytes_ = 0, outputBytes_ = 0;
    UINT64 fenceValue_ = 0, frequency_ = 0;
    bool configured_ = false, poisoned_ = false;
    Timing timing_;
};