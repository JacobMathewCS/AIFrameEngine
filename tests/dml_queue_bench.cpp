#include "../src/dml_gpu_io.h"
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <thread>

// Explicit hardware experiment, not an unconditional CTest. Identical synthetic
// inputs and 30 Hz demand in each trial; overdue slots are skipped, never queued.
int wmain(int argc, wchar_t** argv) {
    try {
        if (argc != 5) throw std::runtime_error("usage: DmlQueueBench model adapter seconds output.csv");
        const int adapter = std::stoi(argv[2]), seconds = std::stoi(argv[3]);
        if (adapter < 0 || seconds < 1 || seconds > 180) throw std::invalid_argument("invalid adapter/duration");
        std::ofstream csv{std::filesystem::path(argv[4])};
        if (!csv) throw std::runtime_error("cannot create CSV");
        Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "QueueComparison");
        DmlGpuIo gpu(static_cast<UINT>(adapter));
        Ort::SessionOptions options;
        options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
        gpu.AppendProvider(options);
        Ort::Session session(env, argv[1], options);
        if (session.GetInputCount() != 2 || session.GetOutputCount() != 1)
            throw std::runtime_error("expected two inputs and one output");
        auto inputInfo = session.GetInputTypeInfo(0);
        auto outputInfo = session.GetOutputTypeInfo(0);
        const auto in = inputInfo.GetTensorTypeAndShapeInfo().GetShape();
        const auto out = outputInfo.GetTensorTypeAndShapeInfo().GetShape();
        if (in.size() != 4 || out.size() != 4 || in[0] != 1 || in[1] != 3 ||
            out[0] != 1 || out[2] != in[2] || out[3] != in[3])
            throw std::runtime_error("invalid NCHW model");
        gpu.Configure(in[3], in[2], out[1]);
        std::vector<float> a(static_cast<size_t>(in[2] * in[3] * 3)), b(a.size());
        for (size_t i = 0; i < a.size(); ++i) {
            a[i] = static_cast<float>(i % 251) / 250.0f;
            b[i] = static_cast<float>((i * 7) % 251) / 250.0f;
        }
        Ort::AllocatorWithDefaultOptions allocator;
        auto name = session.GetOutputNameAllocated(0, allocator);
        for (int i = 0; i < 5; ++i) gpu.Run(session, Ort::RunOptions{nullptr}, a, b, name.get());
        using Clock = std::chrono::steady_clock;
        LARGE_INTEGER frequency{}, originBefore{}, originAfter{};
        QueryPerformanceFrequency(&frequency);
        QueryPerformanceCounter(&originBefore);
        const auto start = Clock::now();
        QueryPerformanceCounter(&originAfter);
        const auto deadline = start + std::chrono::seconds(seconds);
        const auto interval = std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(1.0 / 30.0));
        auto next = start;
        unsigned run = 0, missed = 0;
        csv << "run,start_s,wall_ms,host_run_ms,finish_wait_ms,upload_queue_ms,inference_queue_ms,readback_queue_ms,missed_slots,pid,qpc_frequency,origin_qpc_before,origin_qpc_after,run_qpc_before,run_qpc_after\n";
        csv << std::fixed << std::setprecision(6);
        while (Clock::now() < deadline) {
            std::this_thread::sleep_until(next);
            const auto before = Clock::now();
            if (before >= deadline) break;
            LARGE_INTEGER runBefore{}, runAfter{};
            QueryPerformanceCounter(&runBefore);
            const auto& output = gpu.Run(session, Ort::RunOptions{nullptr}, a, b, name.get());
            QueryPerformanceCounter(&runAfter);
            const auto after = Clock::now();
            for (float v : output) if (!std::isfinite(v)) throw std::runtime_error("nonfinite output");
            const auto& t = gpu.LastTiming();
            if (t.inferenceQueueMs < 0) throw std::runtime_error("invalid queue timestamps");
            next += interval;
            while (next < after) { next += interval; ++missed; }
            csv << run++ << ',' << std::chrono::duration<double>(before-start).count() << ','
                << std::chrono::duration<double,std::milli>(after-before).count() << ','
                << t.runCpuMs << ',' << t.finishWaitMs << ',' << t.uploadQueueMs << ','
                << t.inferenceQueueMs << ',' << t.readbackQueueMs << ',' << missed << ','
                << GetCurrentProcessId() << ',' << frequency.QuadPart << ','
                << originBefore.QuadPart << ',' << originAfter.QuadPart << ','
                << runBefore.QuadPart << ',' << runAfter.QuadPart << '\n';
        }
        csv.flush();
        if (!csv || !run) throw std::runtime_error("missing benchmark samples/write failure");
        std::cout << "PASS samples=" << run << " missed_slots=" << missed << '\n';
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}