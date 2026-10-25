#include "../src/worker_cpu_probe.h"
#include <cstdlib>
#include <iostream>

static void Check(bool ok, const char* message) {
    if (!ok) { std::cerr << message << '\n'; std::exit(1); }
}

int main() {
    Check(!ParseWorkerCpuProbe(nullptr), "default off");
    for (const char* bad : {"", "0", "2", "01", "1x", " 1", "true"})
        Check(!ParseWorkerCpuProbe(bad), "strict opt-in");
    Check(ParseWorkerCpuProbe("1"), "explicit opt-in");
    unsigned reads = 0;
    auto reader = [&reads]() { ++reads; return WorkerCpuSample{}; };
    SampleWorkerCpu(false, reader);
    Check(reads == 0, "disabled must not call Windows sampling APIs");
    SampleWorkerCpu(true, reader);
    Check(reads == 1, "enabled samples once");
    FILETIME filetime{0xffffffffu, 1};
    Check(WorkerFileTime(filetime) == 0x1ffffffffULL, "unsigned FILETIME conversion");
    WorkerCpuSample a{true, 1, 2, 0, 100, 102, 10000000, 123, 500, 600};
    WorkerCpuSample b{true, 1, 2, 0, 200, 202, 10000000, 123, 510, 620};
    Check(WorkerCpuDeltaValid(a, b), "valid outer bracket");
    const auto text = WorkerCpuRecord(7, a, b);
    Check(text.find("kernel_delta_100ns=10 user_delta_100ns=20") != std::string::npos,
          "integer deltas");
    Check(text.find("gpu_duration_ms=null") != std::string::npos, "no GPU attribution");
    for (unsigned field = 0; field < 10; ++field) {
        auto bad = b;
        switch (field) {
        case 0: bad.valid = false; break;
        case 1: ++bad.pid; break;
        case 2: ++bad.tid; break;
        case 3: ++bad.created; break;
        case 4: ++bad.frequency; break;
        case 5: bad.qpcBefore = 101; break;
        case 6: bad.qpcAfter = 199; break;
        case 7: bad.kernel = 499; break;
        case 8: bad.user = 599; break;
        case 9: bad.created = 0; break;
        }
        Check(!WorkerCpuDeltaValid(a, bad), "invalid identity/counters fail closed");
        Check(WorkerCpuRecord(7, a, bad).find("kernel_delta_100ns=null user_delta_100ns=null")
              != std::string::npos, "invalid counters never become zero CPU");
    }
    b.kernel = a.kernel; b.user = a.user;
    Check(WorkerCpuDeltaValid(a, b), "zero delta allowed, not proof of zero execution");
    // Real Windows API smoke check: no workload, sleep or minimum-CPU assertion.
    const auto liveA = ReadWorkerCpuSample(), liveB = ReadWorkerCpuSample();
    Check(WorkerCpuDeltaValid(liveA, liveB), "live identity/QPC/accounting read");
    std::cout << "PASS: opt-in, disabled no-read, identity, monotonicity, nulls, live Windows reads\n";
}