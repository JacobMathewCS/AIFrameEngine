#pragma once
#include <cstdint>
#include "adaptive_latency.h"

// Controls inference submission, NOT presentation. Capture jitter must never
// clear the buffered timeline or switch between delayed and latest frames.
// The budget follows the adaptive presentation delay: admission gets the
// delay minus a 5ms upload margin so a submitted job still fits before its
// pair's retained window expires.
class FrameJobAdmission {
public:
    // Fallback budget when no latency controller is attached (66.7ms - 5ms).
    static constexpr int64_t Budget = 616667;
    static constexpr int64_t RetryInterval = 20000000;

    void Reset() { estimate_ = 0; slowResults_ = 0; cooldown_ = false; nextProbe_ = 0; presentationDelay_ = 0; }
    void SetLatencyController(AdaptiveLatency* latency) { latency_ = latency; }
    void SetPresentationDelay(int64_t delay) { presentationDelay_ = delay; }
    int64_t GetBudget() const {
        // The clock slews toward the controller target. Use the actual playback
        // delay, not a future requested value, for the current pair deadline.
        if (presentationDelay_ > 0) return presentationDelay_ > 50000 ? presentationDelay_ - 50000 : 0;
        return latency_ ? latency_->GetDelay() - 50000 : Budget;
    }

    void Observe(int64_t serviceTime) {
        if (serviceTime <= 0) return;
        estimate_ = estimate_ == 0 ? serviceTime : (7 * estimate_ + serviceTime) / 8;
        const int64_t budget = GetBudget();
        if (serviceTime > budget) {
            if (++slowResults_ >= 3) cooldown_ = true;
        } else {
            slowResults_ = 0;
            // A successful probe must have headroom before normal admission resumes.
            if (cooldown_ && serviceTime < budget * 9 / 10) {
                // Old stall samples must not silently keep normal mode probe-only.
                // Rebase only on a successful recovery probe; normal samples
                // retain EMA smoothing and renewed overruns still enter cooldown.
                estimate_ = serviceTime;
                cooldown_ = false;
            }
        }
    }
    bool ShouldSubmit(int64_t now, int64_t captureB) const {
        const int64_t budget = GetBudget();
        if (now >= captureB + budget) return false;
        if (cooldown_) return now >= nextProbe_; // deliberate measurement probe
        return estimate_ == 0 || estimate_ < captureB + budget - now || now >= nextProbe_;
    }
    void Submitted(int64_t now) { nextProbe_ = now + RetryInterval; }
    bool CoolingDown() const { return cooldown_; }
    int64_t Estimate() const { return estimate_; }
private:
    AdaptiveLatency* latency_ = nullptr;
    int64_t presentationDelay_ = 0;
    int64_t estimate_ = 0;
    int64_t nextProbe_ = 0;
    int slowResults_ = 0;
    bool cooldown_ = false;
};