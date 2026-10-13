#include "../src/rife_io_buffers.h"
#include <dml_provider_factory.h>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <string>

int wmain(int argc, wchar_t** argv) {
    try {
        if (argc < 2) throw std::runtime_error("model path required; optional DirectML device index");
        Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "RifeIoTest");
        Ort::SessionOptions options;
        options.DisableMemPattern();
        options.SetExecutionMode(ExecutionMode::ORT_SEQUENTIAL);
        if (argc > 2) {
            options.AddConfigEntry("session.disable_cpu_ep_fallback", "1");
            Ort::ThrowOnError(OrtSessionOptionsAppendExecutionProvider_DML(options, std::stoi(argv[2])));
        }
        Ort::Session session(env, argv[1], options);
        auto inputInfo = session.GetInputTypeInfo(0);
        auto outputInfo = session.GetOutputTypeInfo(0);
        auto in = inputInfo.GetTensorTypeAndShapeInfo().GetShape();
        auto out = outputInfo.GetTensorTypeAndShapeInfo().GetShape();
        if (in.size() != 4 || out.size() != 4) throw std::runtime_error("NCHW required");
        RifeIoBuffers buffers(in[3], in[2], out[1]);
        auto firstAddress = buffers.First().data();
        auto secondAddress = buffers.Second().data();
        auto outputAddress = buffers.Output().data();
        Ort::AllocatorWithDefaultOptions allocator;
        auto name = session.GetOutputNameAllocated(0, allocator);
        const char* inputNames[] = {"img0", "img1"};
        const char* outputNames[] = {name.get()};
        auto memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        for (int run = 0; run < 4; ++run) {
            for (size_t i = 0; i < buffers.First().size(); ++i) {
                buffers.First()[i] = static_cast<float>((i + run * 13) % 251) / 250.0f;
                buffers.Second()[i] = static_cast<float>((i * 7 + run * 29) % 251) / 250.0f;
            }
            std::array<Ort::Value, 2> inputs = {
                Ort::Value::CreateTensor<float>(memory, buffers.First().data(), buffers.First().size(), in.data(), in.size()),
                Ort::Value::CreateTensor<float>(memory, buffers.Second().data(), buffers.Second().size(), in.data(), in.size())};
            auto reference = session.Run(Ort::RunOptions{nullptr}, inputNames, inputs.data(), 2, outputNames, 1);
            buffers.Run(session, Ort::RunOptions{nullptr}, name.get());
            auto expected = reference[0].GetTensorData<float>();
            if (reference[0].GetTensorTypeAndShapeInfo().GetElementCount() != buffers.Output().size())
                throw std::runtime_error("output size mismatch");
            float maxError = 0;
            for (size_t i = 0; i < buffers.Output().size(); ++i) {
                float actual = buffers.Output()[i];
                if (!std::isfinite(actual) || !std::isfinite(expected[i])) throw std::runtime_error("nonfinite output");
                maxError = (std::max)(maxError, std::abs(actual - expected[i]));
                if (std::abs(actual - expected[i]) > 1e-4f * (1.0f + std::abs(expected[i])))
                    throw std::runtime_error("preallocated output differs from allocating output");
            }
            if (firstAddress != buffers.First().data() || secondAddress != buffers.Second().data() ||
                outputAddress != buffers.Output().data()) throw std::runtime_error("storage moved");
            std::cout << "run=" << run << " max_error=" << maxError << '\n';
        }
        bool rejected = false;
        try { RifeIoBuffers invalid(0, 4, 5); } catch (const std::invalid_argument&) { rejected = true; }
        if (!rejected) throw std::runtime_error("invalid dimensions accepted");
        std::cout << "PASS: reusable input/output parity, repeated writes, stable storage, invalid shape\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}