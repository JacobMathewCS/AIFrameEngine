#include "gpu_submission_owner.h"
#include <vector>

namespace {
void ValidateTensor(const OrtValue* value, const std::vector<int64_t>& shape, const char* memoryName) {
    if (!value) throw std::invalid_argument("null parity tensor");
    Ort::ConstValue tensor(value);
    const auto info = tensor.GetTensorTypeAndShapeInfo();
    if (info.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT || info.GetShape() != shape)
        throw std::invalid_argument("parity tensor shape/type mismatch");
    const OrtMemoryInfo* memory = nullptr; const char* name = nullptr;
    Ort::ThrowOnError(Ort::GetApi().GetTensorMemoryInfo(value, &memory));
    Ort::ThrowOnError(Ort::GetApi().MemoryInfoGetName(memory, &name));
    if (!name || std::string(name) != memoryName) throw std::invalid_argument("parity tensor memory mismatch");
}
}

std::shared_ptr<GpuBoundSession> GpuBoundSession::Create(std::shared_ptr<GpuProviderContext> context,
    const void* modelBytes, size_t modelSize) {
    if (!context || !modelBytes || !modelSize)
        throw std::invalid_argument("session requires provider and nonempty pinned model bytes");
    return std::shared_ptr<GpuBoundSession>(new GpuBoundSession(std::move(context), modelBytes, modelSize));
}

GpuBoundSession::GpuBoundSession(std::shared_ptr<GpuProviderContext> context,
    const void* modelBytes, size_t modelSize) : context_(std::move(context)) {
    session_ = Ort::Session(context_->env_, modelBytes, modelSize, context_->options_);
}

void GpuBoundSession::ValidateFlowContract() {
    if (session_.GetInputCount() != 2 || session_.GetOutputCount() != 1)
        throw std::runtime_error("expected two inputs and one output");
    Ort::AllocatorWithDefaultOptions allocator;
    const std::vector<int64_t> inputShape{1, 3, 320, 576}, outputShape{1, 5, 320, 576};
    for (size_t n = 0; n < 2; ++n) {
        const auto name = session_.GetInputNameAllocated(n, allocator);
        const auto type = session_.GetInputTypeInfo(n);
        const auto tensor = type.GetTensorTypeAndShapeInfo();
        if (std::string(name.get()) != GpuOrtInputs::Names[n] || tensor.GetShape() != inputShape ||
            tensor.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT)
            throw std::runtime_error("FP32 input model contract mismatch");
    }
    const auto name = session_.GetOutputNameAllocated(0, allocator);
    const auto type = session_.GetOutputTypeInfo(0);
    const auto tensor = type.GetTensorTypeAndShapeInfo();
    if (std::string(name.get()) != "flow_mask" || tensor.GetShape() != outputShape ||
        tensor.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT)
        throw std::runtime_error("FP32 output model contract mismatch");
}

void GpuBoundSession::RunForParity(const std::array<const OrtValue*, 2>& inputs, Ort::Value& cpuOutput, bool gpuInputs) {
    // Single-threaded harness only. Preallocated CPU output is an explicit test
    // readback, not a GPU-resident reconstruction implementation. All tensors must
    // stay alive through the caller's ordered retirement fence even when Run throws.
    for (const auto* input : inputs) ValidateTensor(input, {1, 3, 320, 576}, gpuInputs ? "DML" : "Cpu");
    ValidateTensor(cpuOutput, {1, 5, 320, 576}, "Cpu");
    const char* names[]{"img0", "img1"}; const char* outputNames[]{"flow_mask"};
    OrtValue* output = cpuOutput;
    Ort::RunOptions options;
    Ort::ThrowOnError(Ort::GetApi().Run(session_, options, names, inputs.data(), 2, outputNames, 1, &output));
    if (output != static_cast<OrtValue*>(cpuOutput)) std::terminate(); // preallocated output must not be replaced
}

void GpuBoundSession::RunResident(const std::array<const OrtValue*, 2>& inputs, OrtValue* output) {
    // Private to the typed resident owner: context/session identity and retention
    // are established before producer submission. No CPU output allocation here.
    for (const auto* input : inputs) ValidateTensor(input, {1, 3, 320, 576}, "DML");
    ValidateTensor(output, {1, 5, 320, 576}, "DML");
    const char* names[]{"img0", "img1"}; const char* outputNames[]{"flow_mask"};
    OrtValue* bound = output;
    Ort::RunOptions options;
    Ort::ThrowOnError(Ort::GetApi().Run(session_, options, names, inputs.data(), 2, outputNames, 1, &bound));
    if (bound != output) std::terminate();
}