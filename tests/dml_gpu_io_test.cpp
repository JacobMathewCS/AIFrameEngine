#include "../src/dml_gpu_io.h"
#include <algorithm>
#include <cmath>
#include <iostream>

int wmain(int argc, wchar_t** argv) {
    try {
        if (argc != 3) throw std::runtime_error("usage: DmlGpuIoTest model.onnx adapter_index");
        Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "GpuIoParity");
        DmlGpuIo gpu(static_cast<UINT>(std::stoul(argv[2])));
        Ort::SessionOptions options;
        options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
        gpu.AppendProvider(options);
        Ort::Session session(env, argv[1], options);
        auto inputType = session.GetInputTypeInfo(0);
        auto outputType = session.GetOutputTypeInfo(0);
        auto in = inputType.GetTensorTypeAndShapeInfo().GetShape();
        auto out = outputType.GetTensorTypeAndShapeInfo().GetShape();
        if (in.size() != 4 || out.size() != 4 || in[0] != 1 || in[1] != 3 ||
            out[0] != 1 || in[2] != out[2] || in[3] != out[3]) throw std::runtime_error("invalid NCHW contract");
        bool rejected = false;
        try { gpu.Configure(0, in[2], out[1]); } catch (const std::invalid_argument&) { rejected = true; }
        if (!rejected) throw std::runtime_error("invalid dimensions accepted");
        gpu.Configure(in[3], in[2], out[1]);
        std::vector<float> a(static_cast<size_t>(in[2] * in[3] * 3)), b(a.size());
        auto memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        Ort::AllocatorWithDefaultOptions allocator;
        auto name = session.GetOutputNameAllocated(0, allocator);
        const char* names[] = {"img0", "img1"};
        const char* outputs[] = {name.get()};
        const float* stableOutput = nullptr;
        for (int run = 0; run < 6; ++run) {
            for (size_t i = 0; i < a.size(); ++i) {
                a[i] = static_cast<float>((i + run * 13) % 251) / 250.0f;
                b[i] = static_cast<float>((i * 7 + run * 29) % 251) / 250.0f;
            }
            std::array<Ort::Value, 2> inputs = {
                Ort::Value::CreateTensor<float>(memory, a.data(), a.size(), in.data(), 4),
                Ort::Value::CreateTensor<float>(memory, b.data(), b.size(), in.data(), 4)};
            auto reference = session.Run(Ort::RunOptions{nullptr}, names, inputs.data(), 2, outputs, 1);
            const auto& actual = gpu.Run(session, Ort::RunOptions{nullptr}, a, b, name.get());
            if (actual.size() != reference[0].GetTensorTypeAndShapeInfo().GetElementCount()) throw std::runtime_error("size mismatch");
            auto expected = reference[0].GetTensorData<float>();
            float maxError = 0;
            for (size_t i = 0; i < actual.size(); ++i) {
                if (!std::isfinite(actual[i]) || !std::isfinite(expected[i])) throw std::runtime_error("nonfinite result");
                const float error = std::abs(actual[i] - expected[i]);
                maxError = (std::max)(error, maxError);
                if (error > 1e-4f * (1.0f + std::abs(expected[i]))) throw std::runtime_error("GPU/CPU-boundary parity failed");
            }
            if (stableOutput && stableOutput != actual.data()) throw std::runtime_error("storage changed");
            stableOutput = actual.data();
            const auto& t = gpu.LastTiming();
            if (t.inferenceQueueMs < 0 || t.uploadQueueMs < 0 || t.readbackQueueMs < 0) throw std::runtime_error("invalid timestamps");
            std::cout << "run=" << run << " max_error=" << maxError << " run_host_ms=" << t.runCpuMs
                << " finish_wait_ms=" << t.finishWaitMs << " inference_queue_elapsed_ms=" << t.inferenceQueueMs << '\n';
        }
        rejected = false;
        try { gpu.Run(session, Ort::RunOptions{nullptr}, {}, b, name.get()); } catch (const std::exception&) { rejected = true; }
        if (!rejected) throw std::runtime_error("invalid input accepted");
        std::cout << "PASS: GPU tensors, repeated writes, parity, timestamps, invalid inputs\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}