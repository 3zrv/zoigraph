#pragma once

#include <cstddef>
#include <unordered_map>
#include <utility>
#include <vector>

#include "telemetry/phantom.h"

namespace zg::app {

// Result of one diff tick between the previous frame's seen-set and the
// snapshot the phantom_buffer just produced.
//   new_indices = positions in `current` of phantoms whose ids didn't
//                 appear in `seen` last frame -> caller emits phantom_spawn.
//   departed    = (id, lifetime_seconds) pairs for ids that were in
//                 `seen` last frame but aren't in `current` now ->
//                 caller emits phantom_decay. The pin path is expected to
//                 erase the id from `seen` itself before this runs, so
//                 pins don't show up here as decays.
struct LifecycleDelta {
    std::vector<std::size_t>                   new_indices;
    std::vector<std::pair<long long, double>>  departed;
};

// Pure-logic phantom lifecycle diff. Mutates `seen` so it matches the
// id-set of `current` after the call: new ids get inserted with their
// phantom's spawn_time; vanished ids get erased. `now_ts` is used to
// compute lifetime for each departed entry (now_ts - that entry's
// recorded spawn_time).
//
// Caller responsibilities (NOT inside this function):
//   - clearing `seen` on project switch (the diff would otherwise
//     emit a flood of spurious decays into the wrong DB).
//   - erasing the id of a pinned phantom from `seen` before this
//     function runs on the next frame (so the pin path's removal
//     stops it from also showing up here).
//   - actually writing log_event rows from the returned delta.
LifecycleDelta phantom_lifecycle_diff(
    std::unordered_map<long long, double>& seen,
    const std::vector<zg::telemetry::Phantom>& current,
    double now_ts);

}  // namespace zg::app
