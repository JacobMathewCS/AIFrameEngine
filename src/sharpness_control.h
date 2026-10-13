#pragma once
#include <cmath>
#include <initializer_list>

// Application strength, not RCAS sharpness-in-stops. Integer steps avoid drift.
namespace SharpnessControl {
inline int Step(float strength) {
    if (!std::isfinite(strength)) return 12;
    if (strength <= 0.0f) return 0;
    if (strength >= 1.0f) return 20;
    return static_cast<int>(std::lround(strength * 20.0f));
}
inline float Adjust(float strength, bool increase) {
    int step = Step(strength) + (increase ? 1 : -1);
    if (step < 0) step = 0;
    if (step > 20) step = 20;
    return step / 20.0f;
}
inline float NextPreset(float strength) {
    const int step = Step(strength);
    for (int preset : {0, 8, 12, 16, 20}) {
        if (preset > step) return preset / 20.0f;
    }
    return 0.0f;
}
}