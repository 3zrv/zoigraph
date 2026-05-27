#pragma once

#include <chrono>

namespace zg::app {

// Wall-clock Unix seconds. Stable across process restarts (unlike raylib's
// GetTime, which resets each launch), so this is what we persist for
// timestamps like first_seen / last_touched / last_open_ts.
inline double unix_now() {
    using namespace std::chrono;
    return duration<double>(system_clock::now().time_since_epoch()).count();
}

}  // namespace zg::app
