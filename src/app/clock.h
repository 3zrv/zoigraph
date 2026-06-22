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

// Monotonic seconds from std::chrono::steady_clock — thread-safe and
// independent of the render loop, unlike raylib's GetTime() (whose cross-thread
// use is undocumented). The shared "now" for everything that must agree across
// the render, physics, and telemetry threads: phantom spawn times, TTL
// expiry/decay, and time-to-pin. Not for persistence (no fixed epoch) — use
// unix_now() for stored timestamps.
inline double mono_now() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

}  // namespace zg::app
