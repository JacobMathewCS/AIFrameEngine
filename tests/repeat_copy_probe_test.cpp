#include "../src/repeat_copy_probe.h"
#include <cstdlib>
#include <iostream>
#include <memory>

static void Check(bool ok, const char* message) {
    if (!ok) { std::cerr << message << '\n'; std::exit(1); }
}

int main() {
    Check(ParseRepeatCopyProbe(nullptr) == 0, "null defaults off");
    for (const char* bad : {"", "-1", "5", "999999999", "1x", " 1", "01"})
        Check(ParseRepeatCopyProbe(bad) == 0, "invalid dose defaults off");
    for (unsigned dose = 0; dose <= 4; ++dose) {
        const char value[] = {char('0' + dose), '\0'};
        Check(ParseRepeatCopyProbe(value) == dose, "valid dose");
        RepeatCopyProbe<int> probe;
        using Key = RepeatCopyProbe<int>::Key;
        const Key a{10, 100, 1, false, true, 0};
        probe.Configure(dose);
        Check(probe.Copies(true, a) == 0, "first image is not a repeat");
        probe.Commit(true, true, a);
        Check(probe.Copies(true, a) == dose, "repeat gets requested dose");
        Check(probe.Copies(false, a) == 0, "cache miss gets no extra copies");
        for (int field = 0; field < 7; ++field) {
            auto changed = a;
            switch (field) {
            case 0: changed.frame = 20; break;
            case 1: ++changed.content; break;
            case 2: ++changed.revision; break;
            case 3: changed.generated = true; break;
            case 4: changed.fg = false; break;
            case 5: changed.mode = 1; break;
            case 6: changed.frame = 0; break;
            }
            Check(probe.Copies(true, changed) == 0, "changed key never gets a dose");
        }
        // A / midpoint / B transitions never acquire diagnostic copies.
        const Key m{15, 150, 1, true, true, 0}, b{20, 200, 1, false, true, 0};
        Check(probe.Copies(true, m) == 0, "new midpoint");
        probe.Commit(true, true, m);
        Check(probe.Copies(true, m) == dose, "repeated midpoint");
        Check(probe.Copies(true, b) == 0, "new B");
        probe.Commit(true, true, b);
        Check(probe.Copies(true, b) == dose, "repeated B");
        probe.Commit(false, true, b);
        Check(probe.Copies(true, b) == 0, "failed or occluded present invalidates");
        probe.Commit(true, false, a);
        Check(probe.Copies(true, a) == 0, "mutable or uncached image invalidates");
        probe.Commit(true, true, a);
        probe.Configure(99);
        Check(probe.Extra() == 0 && probe.Copies(true, a) == 0, "invalid configure resets");
    }
    RepeatCopyProbe<std::shared_ptr<int>> retained;
    retained.Configure(1);
    auto frame = std::make_shared<int>(42);
    std::weak_ptr<int> lifetime = frame;
    retained.Commit(true, true, {frame, 1, 1, false, true, 0});
    frame.reset();
    Check(!lifetime.expired(), "previous identity stays owned");
    retained.Commit(false, true, {});
    Check(lifetime.expired(), "invalidation releases ownership");
    std::cout << "PASS: bounded opt-in, repeat-only keys, A/M/B, failed presents, ownership\n";
}