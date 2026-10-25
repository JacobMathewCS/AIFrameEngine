#pragma once
#include <cstdint>
#include <deque>
#include <utility>
#include "playback_clock.h"

enum class PairAvailability { Available, Missing, Expired };

inline const char* PairAvailabilityName(PairAvailability value) {
    switch (value) {
    case PairAvailability::Available: return "available";
    case PairAvailability::Expired: return "expired";
    default: return "missing";
    }
}

// Bounded, render-thread-only timeline. Times are in QPC-derived 100ns units.
// A result is attached to its retained pair, never to the newest capture pair.
template<class Frame>
class FrameTimeline {
public:
    struct Selection {
        Frame frame{};
        int64_t pair = 0;
        bool generated = false;
        int64_t contentTime = 0;
    };
    struct Pair {
        int64_t id, aTime, bTime;
        Frame a, b, midpoint{};
    };

    void Clear() { pairs_.clear(); clock_.Reset(); }
    void Add(int64_t id, int64_t aTime, int64_t bTime, Frame a, Frame b) {
        if (bTime <= aTime) return;
        if (!pairs_.empty() && aTime < pairs_.back().bTime) Clear();
        pairs_.push_back({id, aTime, bTime, std::move(a), std::move(b), {}});
        while (pairs_.size() > 8) pairs_.pop_front();
    }
    bool Attach(int64_t id, Frame midpoint, int64_t now, PairAvailability* reason = nullptr) {
        bool found = false;
        for (auto& pair : pairs_) {
            found = found || pair.id == id;
            if (pair.id == id && now - clock_.DelayAt(now) < pair.bTime) {
                pair.midpoint = std::move(midpoint);
                if (reason) *reason = PairAvailability::Available;
                return true;
            }
        }
        if (reason) *reason = found ? PairAvailability::Expired : PairAvailability::Missing;
        return false;
    }
    bool Sources(int64_t id, int64_t now, Frame& a, Frame& b, PairAvailability* reason = nullptr) const {
        bool found = false;
        for (const auto& pair : pairs_) {
            found = found || pair.id == id;
            if (pair.id == id && now - clock_.DelayAt(now) < pair.bTime) {
                if (reason) *reason = PairAvailability::Available;
                a = pair.a; b = pair.b; return true;
            }
        }
        if (reason) *reason = found ? PairAvailability::Expired : PairAvailability::Missing;
        return false;
    }
    Selection Select(int64_t now, bool allowGenerated = true) {
        if (pairs_.empty()) return {};
        const int64_t target = clock_.SelectTime(now);
        while (pairs_.size() > 1 && target >= pairs_.front().bTime)
            pairs_.pop_front();
        const auto& pair = pairs_.front();
        if (target >= pair.bTime) return {pair.b, pair.id, false, pair.bTime};
        const int64_t midpointTime = pair.aTime + (pair.bTime - pair.aTime) / 2;
        if (allowGenerated && target >= midpointTime && pair.midpoint)
            return {pair.midpoint, pair.id, true, midpointTime};
        return {pair.a, pair.id, false, pair.aTime};
    }

    // Set the presentation delay dynamically (in 100ns units)
    void SetDelay(int64_t delay100ns) { clock_.SetDelay(delay100ns); }
    int64_t GetDelay() const { return clock_.RequestedDelay(); }
    int64_t EffectiveDelayAt(int64_t now) const { return clock_.DelayAt(now); }

private:
    std::deque<Pair> pairs_;
    PlaybackClock clock_;
};