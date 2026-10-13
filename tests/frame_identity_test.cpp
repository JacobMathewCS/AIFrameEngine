#include "../src/frame_identity.h"
#include <cstdlib>
#include <iostream>
static void Check(bool ok, const char* text) {
    if (!ok) { std::cerr << text << '\n'; std::exit(1); }
}
int main() {
    FrameIdentityStats s;
    s.BeginSegment(0);
    // 120 successful presents, 30 real + 30 midpoints, 60 repeat submissions.
    for (int i = 0; i < 120; ++i) {
        const int image = i / 2;
        s.Observe(true, true, 1 + image * 166667LL, (image % 2) != 0, 1 + i * 83333LL);
    }
    Check(s.Sample(10000000), "one-second rates available");
    Check(s.Rate().real == 30 && s.Rate().generated == 30 && s.Rate().repeats == 60,
        "repeat presents must not turn 60 identities into 120");
    s.Observe(false, true, 20000000, false, 11000000);
    Check(s.Total().unsuccessful == 1 && s.Total().real == 30, "failed/occluded not committed");
    s.Observe(true, false, 0, false, 11000001);
    Check(s.Total().empty == 1, "blank not a real image");
    s.Observe(true, true, 1, false, 11000002);
    Check(s.Total().regressions == 1 && s.Total().real == 30, "old identity cannot become novel");
    s.BeginSegment(12000000);
    s.Observe(true, true, 1, false, 12000001);
    Check(s.Total().repeats == 1 && s.Total().real == 0, "toggle hold retains identity history");
    s.Observe(true, true, 20000000, false, 12000002);
    s.Observe(true, true, 20000000, false, 12000003);
    Check(s.Total().real == 1 && s.Total().repeats == 2, "shared pair boundary deduplicated");
    Check(!s.Sample(11000000), "nonpositive clock delta ignored");
    FrameIdentityStats off;
    off.BeginSegment(0);
    for (int i = 0; i < 120; ++i)
        off.Observe(true, true, 1 + (i / 4) * 333333LL, false, 1 + i * 83333LL);
    off.Sample(10000000);
    Check(off.Rate().real == 30 && off.Rate().generated == 0 && off.Rate().repeats == 90,
        "FG off 30 source identities at 120 presents");
    off.Sample(20000000);
    Check(off.Rate().real == 0 && off.Rate().generated == 0, "idle rates decay to zero");
    std::cout << "PASS: frame identities, repeats, failures, boundaries, toggles, rates\n";
}