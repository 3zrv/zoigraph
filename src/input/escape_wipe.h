#pragma once

#include <array>

namespace zg::input {

// Triple-Escape wipe state machine. Records each Escape press timestamp in
// a 3-slot circular buffer; reports true on the press that completes a
// triple within `window` seconds. Pure data + arithmetic — no globals, no
// time source — so it's directly unit-testable with arbitrary fake clocks.
//
// When SQLCipher lands, the trigger is what zeros the key buffer and
// force-closes the DB; today it just signals a clean shutdown.
struct EscapeWipe {
    std::array<double, 3> stamps{{-1e9, -1e9, -1e9}};
    int                   next_slot = 0;

    // Records an Escape press at time `t` (any monotonically-increasing
    // clock; raylib's GetTime works). Returns true when the three most
    // recent presses all fall within `window` seconds of each other.
    bool record(double t, double window);

    // Number of recorded presses still within `window` seconds of `now`,
    // capped at 3. Used to render progress feedback while the operator is
    // mid-triple — "ESC 1/3", "ESC 2/3" — so they can tell the keys are
    // landing.
    int count_recent(double now, double window) const;
};

}  // namespace zg::input
