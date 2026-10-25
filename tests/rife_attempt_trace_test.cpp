#include "../src/rife_attempt_trace.h"
#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

static int calls = 0;
static int64_t tick = 100;
static int64_t Clock() { ++calls; return ++tick; }
static uint32_t Tid() { ++calls; return 42; }
static void Check(bool ok, const char* message) {
    if (!ok) { std::cerr << message << '\n'; std::exit(1); }
}
int main() {
    RifeAttemptTrace disabled;
    { RifeAttemptTrace::Scope s(disabled, 70, "cpu_reuse", Clock, Tid); s.Run(8); }
    Check(calls == 0, "disabled invokes no sampling callbacks");
    RifeAttemptTrace trace;
    trace.Enable(3);
    {
        RifeAttemptTrace::Scope s(trace, 70, "cpu_reuse", Clock, Tid);
        s.Run(8); s.DispatchStart(Clock()); s.DispatchEnd(Clock()); s.Outcome("output_ready");
    }
    { RifeAttemptTrace::Scope s(trace, 71, "cpu_reuse", Clock, Tid); s.Outcome("invalid_input"); }
    try {
        RifeAttemptTrace::Scope s(trace, 72, "cpu_allocating", Clock, Tid);
        s.Run(9); s.DispatchStart(Clock());
        throw std::runtime_error("synthetic");
    } catch (const std::exception&) { }
    { RifeAttemptTrace::Scope s(trace, 73, "gpu_bridge", Clock, Tid); s.Outcome("exception"); }
    std::ostringstream out;
    Check(trace.Write(out, 123), "shutdown serialization");
    const auto text = out.str();
    Check(text.find("attempt,1,123,42,1,70,8,cpu_reuse,output_ready,") != std::string::npos, "pair/run/attempt independent");
    Check(text.find("attempt,1,123,42,2,71,0,cpu_reuse,invalid_input,") != std::string::npos, "early return has no run");
    Check(text.find("attempt,1,123,42,3,72,9,cpu_allocating,unwound,") != std::string::npos, "exception unwinding recorded");
    Check(text.substr(text.size() - 3) == ",1\n", "overflow explicit");
    std::istringstream lines(text);
    std::string line;
    int count = 0;
    while (std::getline(lines, line)) { Check(std::count(line.begin(), line.end(), ',') == 13, "14 columns"); ++count; }
    Check(count == 5, "header bounded rows footer");
    std::ostringstream failed;
    failed.setstate(std::ios::badbit);
    Check(!trace.Write(failed, 123), "write failure surfaced");
    std::cout << "PASS: disabled sampling, identity, early return, unwind, overflow, shutdown\n";
}