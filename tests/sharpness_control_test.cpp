#include "../src/sharpness_control.h"
#include <cstdlib>
#include <iostream>
#include <limits>
static void Check(bool ok, const char* message) {
    if (!ok) { std::cerr << message << '\n'; std::exit(1); }
}
int main() {
    using namespace SharpnessControl;
    Check(Adjust(0.6f, true) == 0.65f, "fine increase");
    Check(Adjust(0.6f, false) == 0.55f, "fine decrease");
    float value = 0.0f;
    for (int n = 1; n <= 20; ++n) {
        value = Adjust(value, true);
        Check(value == n / 20.0f, "ascending steps without drift");
    }
    Check(Adjust(value, true) == 1.0f, "upper clamp");
    for (int n = 19; n >= 0; --n) {
        value = Adjust(value, false);
        Check(value == n / 20.0f, "descending steps without drift");
    }
    Check(Adjust(value, false) == 0.0f, "lower clamp");
    for (float expected : {0.4f, 0.6f, 0.8f, 1.0f, 0.0f}) {
        value = NextPreset(value);
        Check(value == expected, "legacy preset sequence");
    }
    Check(NextPreset(0.65f) == 0.8f, "preset after fine adjustment");
    Check(NextPreset(0.55f) == 0.6f, "preset follows current value");
    Check(Adjust(-100.0f, false) == 0.0f, "invalid low clamp");
    Check(Adjust(100.0f, true) == 1.0f, "invalid high clamp");
    Check(Step(std::numeric_limits<float>::quiet_NaN()) == 12, "nonfinite default");
    std::cout << "PASS: fine steps, clamps, presets, and mixed controls\n";
}