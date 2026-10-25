#pragma once
#include <cstdint>

// Render-thread diagnostics only. Identity is content time + frame type, NOT
// pair ID (B of one pair is A of the next), texture address, or pixel uniqueness.
class FrameIdentityStats {
public:
    struct Counts {
        uint64_t real = 0, generated = 0, repeats = 0, regressions = 0;
        uint64_t unsuccessful = 0, empty = 0;
    };
    struct Rates {
        double real = 0, generated = 0, repeats = 0;
        double changeMinMs = 0, changeMaxMs = 0;
    };
    void BeginSegment(int64_t now) {
        ++segment_; counts_ = {}; rates_ = {}; window_ = {};
        windowStart_ = now; lastChange_ = 0; minChange_ = maxChange_ = 0;
        // Preserve high-water marks: enabling FG can hold the already-shown image.
    }
    void Observe(bool success, bool hasImage, int64_t contentTime, bool generated, int64_t now) {
        if (!success) { ++counts_.unsuccessful; return; }
        if (!hasImage || contentTime <= 0) { ++counts_.empty; return; }
        if (lastTime_ && contentTime < lastTime_) ++counts_.regressions;
        auto& high = generated ? highGenerated_ : highReal_;
        if (contentTime <= high) {
            ++counts_.repeats;
        } else {
            high = contentTime;
            if (generated) ++counts_.generated; else ++counts_.real;
            if (lastChange_ && now > lastChange_) {
                const int64_t dt = now - lastChange_;
                if (!minChange_ || dt < minChange_) minChange_ = dt;
                if (dt > maxChange_) maxChange_ = dt;
            }
            lastChange_ = now;
        }
        lastTime_ = contentTime;
    }
    bool Sample(int64_t now) {
        if (now <= windowStart_ || now - windowStart_ < 10000000) return false;
        const double seconds = double(now - windowStart_) / 10000000.0;
        rates_.real = (counts_.real - window_.real) / seconds;
        rates_.generated = (counts_.generated - window_.generated) / seconds;
        rates_.repeats = (counts_.repeats - window_.repeats) / seconds;
        rates_.changeMinMs = minChange_ / 10000.0;
        rates_.changeMaxMs = maxChange_ / 10000.0;
        window_ = counts_; windowStart_ = now; minChange_ = maxChange_ = 0;
        return true;
    }
    const Counts& Total() const { return counts_; }
    const Rates& Rate() const { return rates_; }
    uint64_t Segment() const { return segment_; }
private:
    Counts counts_{}, window_{};
    Rates rates_{};
    uint64_t segment_ = 0;
    int64_t highReal_ = 0, highGenerated_ = 0, lastTime_ = 0;
    int64_t windowStart_ = 0, lastChange_ = 0, minChange_ = 0, maxChange_ = 0;
};