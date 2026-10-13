#pragma once
#include <cstdint>

// Render-thread clock in 100ns units. Delay changes adjust playback speed by
// at most 5%, instead of stepping the content timestamp forward/backward.
// Deadline queries are projections: only presentation selection commits time.
class PlaybackClock {
public:
    void SetDelay(int64_t delay) { requested_ = delay < 0 ? 0 : delay; }
    int64_t RequestedDelay() const { return requested_; }
    void Reset() { initialized_ = false; }
    int64_t DelayAt(int64_t now) const {
        if (!initialized_) return requested_;
        if (now <= lastNow_) return effective_;
        const int64_t step = (now - lastNow_) / 20;
        if (requested_ > effective_)
            return effective_ + ((requested_ - effective_ < step) ? requested_ - effective_ : step);
        return effective_ - ((effective_ - requested_ < step) ? effective_ - requested_ : step);
    }
    int64_t SelectTime(int64_t now) {
        if (initialized_ && now <= lastNow_) return lastTarget_;
        effective_ = DelayAt(now);
        lastNow_ = now;
        lastTarget_ = now - effective_;
        initialized_ = true;
        return lastTarget_;
    }
private:
    int64_t requested_ = 666667;
    int64_t effective_ = 666667;
    int64_t lastNow_ = 0, lastTarget_ = 0;
    bool initialized_ = false;
};