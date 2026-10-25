#pragma once
#include <windows.h>
#include <cstdint>
#include <string>

// Calling-thread accounting only. Never interpret wall minus CPU as GPU/wait time.
inline bool ParseWorkerCpuProbe(const char* value) {
    return value && value[0] == '1' && value[1] == '\0';
}

struct WorkerCpuSample {
    bool valid = false;
    DWORD pid = 0, tid = 0, error = 0;
    int64_t qpcBefore = 0, qpcAfter = 0, frequency = 0;
    uint64_t created = 0, kernel = 0, user = 0;
};

inline uint64_t WorkerFileTime(const FILETIME& value) {
    return (uint64_t(value.dwHighDateTime) << 32) | value.dwLowDateTime;
}

inline WorkerCpuSample ReadWorkerCpuSample() {
    WorkerCpuSample sample;
    sample.pid = GetCurrentProcessId();
    sample.tid = GetCurrentThreadId();
    LARGE_INTEGER frequency{}, before{}, after{};
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!QueryPerformanceFrequency(&frequency) || !QueryPerformanceCounter(&before)) {
        sample.error = GetLastError();
        return sample;
    }
    const BOOL ok = GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user);
    if (!ok) sample.error = GetLastError();
    const BOOL clockOk = QueryPerformanceCounter(&after);
    if (!clockOk) sample.error = GetLastError();
    sample.frequency = frequency.QuadPart;
    sample.qpcBefore = before.QuadPart;
    sample.qpcAfter = after.QuadPart;
    sample.created = WorkerFileTime(created);
    sample.kernel = WorkerFileTime(kernel);
    sample.user = WorkerFileTime(user);
    sample.valid = ok && clockOk && sample.frequency > 0 &&
        sample.qpcBefore > 0 && sample.qpcAfter >= sample.qpcBefore && sample.created > 0;
    return sample;
}

template<class Reader>
inline WorkerCpuSample SampleWorkerCpu(bool enabled, Reader reader) {
    return enabled ? reader() : WorkerCpuSample{};
}

inline bool WorkerCpuDeltaValid(const WorkerCpuSample& a, const WorkerCpuSample& b) {
    return a.valid && b.valid && a.pid && a.tid && a.created &&
        a.pid == b.pid && a.tid == b.tid && a.created == b.created &&
        a.frequency > 0 && a.frequency == b.frequency &&
        a.qpcBefore > 0 && a.qpcAfter >= a.qpcBefore &&
        b.qpcBefore >= a.qpcAfter && b.qpcAfter >= b.qpcBefore &&
        b.kernel >= a.kernel && b.user >= a.user;
}

inline std::string WorkerCpuRecord(uint64_t runId, const WorkerCpuSample& a,
                                   const WorkerCpuSample& b) {
    const bool valid = WorkerCpuDeltaValid(a, b);
    std::string text = "WORKER CPU: version=1 run_id=" + std::to_string(runId)
        + " valid=" + (valid ? "1" : "0");
    const auto append = [&text](const char* prefix, const WorkerCpuSample& s) {
        text += std::string(" ") + prefix + "_valid=" + (s.valid ? "1" : "0")
            + " " + prefix + "_pid=" + std::to_string(s.pid)
            + " " + prefix + "_tid=" + std::to_string(s.tid)
            + " " + prefix + "_created_filetime=" + std::to_string(s.created)
            + " " + prefix + "_qpc_before=" + std::to_string(s.qpcBefore)
            + " " + prefix + "_qpc_after=" + std::to_string(s.qpcAfter)
            + " " + prefix + "_qpc_frequency=" + std::to_string(s.frequency)
            + " " + prefix + "_kernel_100ns=" + std::to_string(s.kernel)
            + " " + prefix + "_user_100ns=" + std::to_string(s.user)
            + " " + prefix + "_error=" + std::to_string(s.error);
    };
    append("begin", a);
    append("end", b);
    text += " kernel_delta_100ns=" + (valid ? std::to_string(b.kernel - a.kernel) : "null")
        + " user_delta_100ns=" + (valid ? std::to_string(b.user - a.user) : "null")
        + " scope=calling_thread_outer_bracket execution_visibility_verified=0 gpu_duration_ms=null";
    return text;
}