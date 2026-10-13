#include "../src/frame_timeline.h"
#include "../src/frame_job_admission.h"
#include "../src/presentation_guard.h"
#include "../src/latest_frame_request.h"
#include "../src/frame_clock.h"
#include <cstdlib>
#include <iostream>

static void Check(bool ok, const char* message) {
    if (!ok) { std::cerr << message << '\n'; std::exit(1); }
}

int main() {
    Check(QpcTo100ns(123456789, 10000000) == 123456789, "10MHz QPC units");
    Check(QpcTo100ns(30000000000000LL, 3000000) == 100000000000000LL, "long uptime conversion");
    Check(QpcTo100ns(4500000, 3000000) == 15000000, "fractional second conversion");
    CaptureClockAudit audit;
    audit.Observe(1000000, 990000);
    audit.Observe(1000000, 1020000);
    Check(audit.samples == 2 && audit.futureSamples == 1 && audit.minAge == -10000 &&
        audit.maxAge == 20000 && audit.lastAge == 20000, "preserve signed capture timestamp anomaly");
    FrameTimeline<int> timeline;
    constexpr int64_t t = 333334;
    const int64_t delay = timeline.GetDelay();
    Check(!timeline.Select(0).frame, "empty timeline");
    timeline.Add(1, 0, t, 10, 20);
    int sourceA = 0, sourceB = 0;
    Check(timeline.Sources(1, t, sourceA, sourceB) && sourceA == 10 && sourceB == 20, "flow reconstruction source pair");
    Check(!timeline.Sources(99, t, sourceA, sourceB), "unknown reconstruction pair");
    Check(!timeline.Sources(1, delay+t, sourceA, sourceB), "expired reconstruction pair");
    // Result arrives 25ms after B; a newer capture must not invalidate it.
    timeline.Add(2, t, 2*t, 20, 30);
    Check(timeline.Attach(1, 15, t + 250000), "retain old pair result");
    Check(timeline.Select(delay).frame == 10, "A at target zero");
    auto midpoint = timeline.Select(delay + t/2);
    Check(midpoint.generated && midpoint.frame == 15, "midpoint at half interval");
    Check(timeline.Select(delay + t).frame == 20, "B/next A continuity");
    Check(!timeline.Attach(1, 15, delay + t), "reject expired result");
    Check(timeline.Select(delay + t + t/2).frame == 20, "hold A on missing midpoint");
    Check(timeline.Select(delay + 2*t).frame == 30, "never replay old midpoint after B");
    timeline.Clear();
    Check(!timeline.Attach(2, 25, 0), "toggle flush rejects in-flight results");
    timeline.Add(3, t, t, 1, 2);
    Check(!timeline.Select(0).frame, "reject invalid time order");
    for (int64_t i = 0; i < 20; ++i) timeline.Add(i+10, i*t, (i+1)*t, 1, 2);
    Check(!timeline.Attach(10, 3, 0), "bounded history");
    timeline.Clear();
    int generated = 0;
    int64_t lastPair = -1;
    // Simulate 30Hz captures, 25ms inference, 60Hz render polling. Observe
    // midpoints after a newer capture has arrived, as in the actual app.
    for (int tick = 0; tick <= 125; ++tick) {
        const int64_t now = tick * t / 2;
        if (tick % 2 == 0 && tick >= 2) {
            const int64_t id = tick / 2;
            timeline.Add(id, (id-1)*t, id*t, static_cast<int>(id*2), static_cast<int>(id*2+2));
            if (id > 1) Check(timeline.Attach(id-1, static_cast<int>(id*2-1), now), "attach after newer capture");
        }
        auto selection = timeline.Select(now);
        if (selection.generated && selection.pair != lastPair) {
            ++generated;
            lastPair = selection.pair;
        }
    }
    Check(generated >= 59, "sustained simulated midpoint presentation");
    FrameJobAdmission admission;
    // Adaptive delay: capture-to-completion observations, not service alone.
    AdaptiveLatency latency;
    latency.Observe(310000); // 26ms worker + 5ms readback, submitted promptly
    Check(latency.GetDelay() == 310000 + 100000, "delay = worst e2e + 10ms margin");
    latency.Observe(20000000); // > 2x maxDelay sample is rejected as an outlier
    Check(latency.GetDelay() == 310000 + 100000, "outlier beyond 2x ceiling rejected");
    FrameJobAdmission wired;
    wired.SetLatencyController(&latency);
    Check(wired.GetBudget() == latency.GetDelay() - 50000,
        "budget follows adaptive delay minus 5ms upload margin");
    wired.Observe(310000);
    Check(!wired.CoolingDown(), "wired admission accepts in-budget service");
    wired.Observe(5000000); // far beyond the adaptive budget
    wired.Observe(5000000);
    wired.Observe(5000000);
    Check(wired.CoolingDown(), "wired admission cools on adaptive-budget overrun");
    // Fresh controller: min clamp, spike flag, ceiling, worst decay, reset.
    AdaptiveLatency ctl;
    ctl.Observe(50000); // 5ms end-to-end
    Check(ctl.GetDelay() == 166667, "delay floors at 16.7ms minimum");
    ctl.Observe(300000);
    ctl.Observe(300000);
    ctl.Observe(300000);
    ctl.Observe(300000); // EMA converges: 112500, 159375, 194531, 220898
    Check(!ctl.IsThermalSpike(), "steady samples are not a thermal spike");
    ctl.Observe(600000); // sudden 2x jump
    Check(ctl.IsThermalSpike(), "sudden 2x end-to-end jump flags thermal spike");
    Check(ctl.GetWorstService() == 600000, "worst tracks the spike");
    ctl.Observe(300000);
    Check(ctl.GetWorstService() == 570000, "worst decays 10 percent per sample");
    ctl.Observe(1200000); // accepted (under 2x ceiling) but delay clamps
    Check(ctl.GetDelay() == 666667, "delay clamps at 66.7ms maximum");
    for (int i = 0; i < 130; ++i) ctl.Observe(300000);
    Check(ctl.GetWorstService() == 300000, "worst converges to steady samples");
    Check(!ctl.IsThermalSpike(), "spike flag clears once worst matches EMA");
    Check(ctl.GetDelay() == 400000, "recovered delay equals steady worst plus margin");
    ctl.Reset();
    Check(ctl.GetDelay() == 666667 && !ctl.IsThermalSpike(),
        "reset restores conservative ceiling");
    admission.Observe(310000); // 26ms worker + 5ms readback
    timeline.Clear();
    int64_t a = 0;
    for (int64_t id = 1; id <= 100; ++id) {
        const int64_t b = a + (id % 2 ? 208333 : 416667);
        timeline.Add(id, a, b, static_cast<int>(2*id), static_cast<int>(2*id+2));
        Check(admission.ShouldSubmit(b, b), "20.8/41.7ms jitter must not switch admission");
        admission.Submitted(b);
        admission.Observe(310000);
        Check(timeline.Attach(id, static_cast<int>(2*id+1), b + 310000), "jitter must retain in-flight pair");
        auto sample = timeline.Select(a + (b-a)/2 + delay);
        Check(sample.generated && sample.pair == id, "jitter midpoint timeline");
        a = b;
    }
    admission.Reset();
    admission.Submitted(100);
    admission.Observe(700000);
    Check(!admission.CoolingDown(), "one spike must not trigger cooldown");
    Check(admission.ShouldSubmit(20000100, 20000100), "one slow sample cannot permanently lock out");
    admission.Observe(700000);
    admission.Observe(700000);
    Check(admission.CoolingDown(), "sustained overruns trigger cooldown");
    admission.Submitted(20000100);
    Check(!admission.ShouldSubmit(21000100, 21000100), "retry not bypassed by longer source interval");
    Check(admission.ShouldSubmit(40000100, 40000100), "retry after two seconds");
    admission.Observe(300000);
    Check(!admission.CoolingDown(), "good probe exits cooldown");
    // Reproduce the live 180ms stall followed by a 30ms recovery probe.
    admission.Reset();
    admission.Observe(1800000);
    admission.Observe(1800000);
    admission.Observe(1800000);
    Check(admission.CoolingDown(), "long stalls enter probe mode");
    const int64_t probeTime = 50000000;
    admission.Submitted(probeTime);
    admission.Observe(300000);
    Check(!admission.CoolingDown(), "fast probe recovers after long stalls");
    Check(admission.Estimate() == 300000, "recovery estimate must not retain obsolete stall costs");
    const int64_t nextCapture = probeTime + t;
    Check(admission.ShouldSubmit(nextCapture + 100000, nextCapture),
        "recovery admits next useful pair without a two-second wait");
    Check(!admission.ShouldSubmit(nextCapture + 400000, nextCapture),
        "recovered estimate still rejects insufficient remaining time");
    admission.Observe(1800000);
    admission.Observe(1800000);
    admission.Observe(1800000);
    Check(admission.CoolingDown(), "renewed sustained stalls restore cooldown");
    admission.Observe(FrameJobAdmission::Budget * 9 / 10);
    Check(admission.CoolingDown(), "probe at headroom boundary does not recover");
    Check(!admission.ShouldSubmit(probeTime + FrameJobAdmission::RetryInterval,
        probeTime), "expired pair rejected even when a probe is due");
    admission.Reset();
    Check(!admission.ShouldSubmit(delay, 0), "expired normal job rejected");
    // Overload must not require clearing history. A missed midpoint holds A,
    // then advances to B without jumping to the newest capture and back.
    timeline.Clear();
    timeline.Add(1000, 0, t, 100, 200);
    timeline.Add(1001, t, 2*t, 200, 300);
    Check(timeline.Select(delay+t/2).frame == 100, "skipped job retains A");
    Check(timeline.Select(delay+t).frame == 200, "skipped job retains B");
    timeline.Clear();
    timeline.Add(2000, 0, t, 10, 20);
    Check(timeline.Attach(2000, 15, t), "diagnostic midpoint attachment");
    auto realOnly = timeline.Select(delay+t/2, false);
    Check(realOnly.frame == 10 && !realOnly.generated && realOnly.contentTime == 0,
        "real-only mode hides midpoint without relabeling it as generated");
    auto normal = timeline.Select(delay+t/2);
    Check(normal.frame == 15 && normal.contentTime == t/2 && normal.generated,
        "diagnostic does not destroy attached midpoint");
    PresentationGuard<int> guard;
    guard.Commit({20, 2000, false, t}); // last actual frame before F9
    Check(guard.Choose(normal).frame == 20, "F9 cannot replay earlier midpoint");
    Check(guard.Choose({}).frame == 20, "hold last image while timeline fills");
    auto next = guard.Choose({25, 2001, true, t+t/2});
    Check(next.frame == 25, "forward progress after buffer fills");
    Check(guard.Choose(normal).frame == 20, "selection alone does not commit failed Present");
    guard.Commit(next);
    Check(guard.Choose({20, 2001, false, t}).frame == 25, "late results cannot reverse playback");
    guard.Commit({30, 2002, false, 2*t}); // switch to diagnostic real-only
    Check(!guard.Choose(normal).generated, "mode switch clears retained generated image");
    guard.Clear();
    Check(!guard.Choose({}).frame, "guard reset");
    // The retry mailbox must not double-count or resurrect cleared requests.
    LatestFrameRequest request;
    Check(!request.Pending(7), "empty mailbox holds nothing");
    Check(!request.Offer(7), "first offer is never a supersede");
    Check(request.Pending(7) && !request.Pending(8), "only the newest pair is tracked");
    Check(!request.Retried(), "fresh request is a first attempt");
    Check(request.MarkBusy(), "busy is counted once per pair");
    Check(!request.MarkBusy(), "repeated busy ticks do not inflate counters");
    Check(request.Retried(), "busy marks the pair as a retry");
    Check(request.MarkDenied(), "admission denial counted once per pair");
    Check(!request.MarkDenied(), "repeated denial ticks do not inflate counters");
    Check(!request.Offer(7), "same pair re-offered is not a supersede");
    Check(request.Retried(), "same pair must keep its retry state");
    Check(request.Offer(8), "a newer capture supersedes the pending pair");
    Check(!request.Retried(), "superseding resets retry state");
    request.Clear();
    Check(!request.Pending(8) && !request.Retried(), "clear drops the request");
    Check(request.MarkBusy(), "state resets after clear");
    Check(!request.Offer(9), "re-offer after clear is a first attempt");
    std::cout << "PASS: buffered A/M/B, diagnostics, monotonic presentation, pair identity, late/missing results, reset, bounded history, retry mailbox\n";
}