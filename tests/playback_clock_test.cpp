#include "../src/frame_timeline.h"
#include "../src/frame_job_admission.h"
#include <cstdlib>
#include <iostream>

static void Check(bool ok, const char* message) {
    if (!ok) { std::cerr << message << '\n'; std::exit(1); }
}

int main() {
    PlaybackClock clock;
    clock.SetDelay(400000);
    int64_t now = 1000000;
    int64_t previous = clock.SelectTime(now);
    Check(previous == 600000, "initial configured delay");
    clock.SetDelay(666667);
    Check(clock.SelectTime(now) == previous, "delay update cannot move time at same instant");
    for (int i = 0; i < 120; ++i) {
        now += 100000;
        const auto prediction = now - clock.DelayAt(now);
        const auto selected = clock.SelectTime(now);
        Check(selected == prediction, "deadline projection matches presentation clock");
        Check(selected - previous >= 95000 && selected - previous <= 100000,
            "increasing delay never freezes or reverses playback");
        previous = selected;
    }
    Check(clock.DelayAt(now) == 666667, "delay increase converges");
    clock.SetDelay(166667);
    for (int i = 0; i < 120; ++i) {
        now += 100000;
        const auto selected = clock.SelectTime(now);
        Check(selected - previous >= 100000 && selected - previous <= 105000,
            "decreasing delay cannot jump across content time");
        previous = selected;
    }
    Check(clock.DelayAt(now) == 166667, "delay decrease converges");
    Check(clock.SelectTime(now - 100000) == previous, "regressing caller time is held");
    clock.Reset();
    Check(clock.SelectTime(0) == -166667, "reset forgets previous playback epoch");
    clock.SetDelay(-1);
    clock.Reset();
    Check(clock.SelectTime(123) == 123, "negative delay clamped");

    FrameTimeline<int> timeline;
    timeline.SetDelay(400000);
    timeline.Add(1, 0, 333334, 10, 20);
    Check(timeline.Attach(1, 15, 400000), "attach midpoint");
    Check(timeline.Select(600000).generated, "midpoint selected");
    timeline.SetDelay(666667);
    Check(timeline.Select(610000).generated, "delay rise cannot revert midpoint to A");
    int a = 0, b = 0;
    Check(!timeline.Sources(1, 750000, a, b), "reconstruction expiry uses effective delay");
    Check(!timeline.Attach(1, 15, 750000), "attachment shares effective deadline");
    FrameJobAdmission admission;
    AdaptiveLatency controller;
    admission.SetLatencyController(&controller);
    admission.SetPresentationDelay(timeline.EffectiveDelayAt(610000));
    Check(admission.GetBudget() == 350500, "admission uses slewed rather than requested delay");
    admission.Reset();
    Check(admission.GetBudget() == 616667, "reset removes stale effective delay");
    std::cout << "PASS: bounded playback speed, monotonic time, delay convergence, effective deadlines\n";
}