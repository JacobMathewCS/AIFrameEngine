#pragma once
#include <cstdint>
#include <array>

// Adaptive latency controller for frame generation.
// Computes the minimum safe presentation delay based on measured
// END-TO-END time from capture of B to result completion (submit age +
// readback + queue + inference + reconstruction submission). Observing
// service alone under-counts: the timeline's attach deadline runs from
// capture time, so the submit age must be inside the budget too.
// This replaces the fixed 66.7ms buffer with a dynamic value.
class AdaptiveLatency {
public:
    // All times in 100ns units (QPC-derived).
    // minDelay: absolute floor (e.g., 16.7ms for 60Hz)
    // maxDelay: absolute ceiling (e.g., 66.7ms for 30Hz source)
    // safetyMargin: extra padding for scheduling jitter (default 10ms);
    // admission budget becomes delay - 5ms = worst_e2e + 5ms.
    AdaptiveLatency(int64_t minDelay = 166667,   // 16.67ms @ 60Hz
                    int64_t maxDelay = 666667,   // 66.67ms @ 30Hz source
                    int64_t safetyMargin = 100000) // 10ms
        : minDelay_(minDelay), maxDelay_(maxDelay), safetyMargin_(safetyMargin) {}

    // Call after each completed inference to update the estimate.
    // endToEnd100ns = completion time - capture time of B (+ recon submit).
    void Observe(int64_t endToEnd100ns) {
        if (endToEnd100ns <= 0 || endToEnd100ns > maxDelay_ * 2) return;

        // EMA of end-to-end time (alpha = 0.25 for faster adaptation to spikes)
        serviceEma_ = serviceEma_ == 0 ? endToEnd100ns
            : (3 * serviceEma_ + endToEnd100ns) / 4;

        // Track worst-case for safety margin
        if (endToEnd100ns > worstService_) {
            worstService_ = endToEnd100ns;
        }

        // Exponentially decay worst-case over time (slow decay)
        if (worstService_ > 0 && endToEnd100ns < worstService_) {
            worstService_ = (9 * worstService_ + endToEnd100ns) / 10;
        }

        // Recompute delay with thermal-aware safety margin
        // Use worst-case + margin, but clamp to [minDelay, maxDelay]
        int64_t target = worstService_ + safetyMargin_;
        if (target < minDelay_) target = minDelay_;
        if (target > maxDelay_) target = maxDelay_;
        currentDelay_ = target;

        // Track if we're in a thermal spike (worst case significantly exceeds EMA)
        thermalSpike_ = (worstService_ > serviceEma_ * 3 / 2);
    }

    int64_t GetDelay() const { return currentDelay_; }
    int64_t GetServiceEma() const { return serviceEma_; }
    int64_t GetWorstService() const { return worstService_; }
    bool IsThermalSpike() const { return thermalSpike_; }

    // For debugging/logging
    int64_t GetMinDelay() const { return minDelay_; }
    int64_t GetMaxDelay() const { return maxDelay_; }
    int64_t GetSafetyMargin() const { return safetyMargin_; }

    void Reset() {
        serviceEma_ = 0;
        worstService_ = 0;
        currentDelay_ = maxDelay_;
        thermalSpike_ = false;
    }

private:
    const int64_t minDelay_;
    const int64_t maxDelay_;
    const int64_t safetyMargin_;

    int64_t serviceEma_ = 0;
    int64_t worstService_ = 0;
    int64_t currentDelay_ = 666667; // Start conservative
    bool thermalSpike_ = false;
};

// CPU presentation-return intervals and raw DXGI counters; not scanout timing.
struct ScanoutStats {
    int64_t presentedFrames = 0;      // Total Present() calls that succeeded
    int64_t generatedFramesPresented = 0; // Generated midpoints that were presented
    int64_t lastPresentQpc = 0;       // QPC of last Present
    int64_t avgPresentInterval = 0;   // Average interval between Presents (100ns)
    int64_t minPresentInterval = 0;
    int64_t maxPresentInterval = 0;
    int presentCount = 0;             // For computing average

    // DXGI frame statistics
    int64_t frameStatsPresentCount = 0;
    int64_t frameStatsSyncRefreshCount = 0;
    int64_t frameStatsPresentRefreshCount = 0;
    bool frameStatsAvailable = false;
};