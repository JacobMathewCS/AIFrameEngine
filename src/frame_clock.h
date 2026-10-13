#pragma once
#include <cstdint>

// Split the conversion so uptime * 10,000,000 cannot overflow int64_t.
// QPC frequencies on Windows are positive; reject invalid test inputs.
inline int64_t QpcTo100ns(int64_t ticks, int64_t frequency) {
    if (ticks < 0 || frequency <= 0) return 0;
    return (ticks / frequency) * 10000000LL
        + (ticks % frequency) * 10000000LL / frequency;
}

// Do not clamp or shift WGC timestamps: retain evidence of clock anomalies.
struct CaptureClockAudit {
    int64_t samples = 0;
    int64_t futureSamples = 0;
    int64_t lastAge = 0;
    int64_t minAge = 0;
    int64_t maxAge = 0;
    void Observe(int64_t capture, int64_t received) {
        lastAge = received - capture;
        if (!samples || lastAge < minAge) minAge = lastAge;
        if (!samples || lastAge > maxAge) maxAge = lastAge;
        ++samples;
        if (lastAge < 0) ++futureSamples;
    }
};