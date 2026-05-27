#include "input/escape_wipe.h"

#include <algorithm>

namespace zg::input {

bool EscapeWipe::record(double t, double window) {
    stamps[next_slot] = t;
    next_slot = (next_slot + 1) % 3;

    const double lo = std::min({stamps[0], stamps[1], stamps[2]});
    const double hi = std::max({stamps[0], stamps[1], stamps[2]});
    return (hi - lo) <= window;
}

int EscapeWipe::count_recent(double now, double window) const {
    int n = 0;
    for (double t : stamps) {
        if (now - t <= window) ++n;
    }
    return n;
}

}  // namespace zg::input
