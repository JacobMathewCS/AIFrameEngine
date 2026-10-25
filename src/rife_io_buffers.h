#pragma once
#include <onnxruntime_cxx_api.h>
#include <array>
#include <cstdint>
#include <stdexcept>
#include <vector>

// Single inference-thread ownership. Ort values borrow these vectors: never
// resize them after construction, and copy results before the next Run.
class RifeIoBuffers {
public:
    RifeIoBuffers(int64_t width, int64_t height, int64_t channels)
        : inputShape_{1, 3, height, width}, outputShape_{1, channels, height, width},
          first_(Count(width, height, 3)), second_(first_.size()),
          output_(Count(width, height, channels)),
          memory_(Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault)),
          inputs_{Tensor(first_, inputShape_), Tensor(second_, inputShape_)},
          outputTensor_(Tensor(output_, outputShape_)) {}
    RifeIoBuffers(const RifeIoBuffers&) = delete;
    RifeIoBuffers& operator=(const RifeIoBuffers&) = delete;
    std::vector<float>& First() { return first_; }
    std::vector<float>& Second() { return second_; }
    const std::vector<float>& Output() const { return output_; }
    void Run(Ort::Session& session, const Ort::RunOptions& options, const char* outputName) {
        const char* names[] = {"img0", "img1"};
        const char* outputs[] = {outputName};
        session.Run(options, names, inputs_.data(), inputs_.size(), outputs, &outputTensor_, 1);
    }
private:
    static size_t Count(int64_t w, int64_t h, int64_t c) {
        if (w <= 0 || h <= 0 || (c != 3 && c != 5) || w > 16384 || h > 16384)
            throw std::invalid_argument("invalid RIFE tensor dimensions");
        return static_cast<size_t>(w) * static_cast<size_t>(h) * static_cast<size_t>(c);
    }
    Ort::Value Tensor(std::vector<float>& data, const std::array<int64_t, 4>& shape) {
        return Ort::Value::CreateTensor<float>(memory_, data.data(), data.size(), shape.data(), shape.size());
    }
    std::array<int64_t, 4> inputShape_, outputShape_;
    std::vector<float> first_, second_, output_;
    Ort::MemoryInfo memory_;
    std::array<Ort::Value, 2> inputs_;
    Ort::Value outputTensor_;
};