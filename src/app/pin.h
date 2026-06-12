#pragma once

#include <unordered_map>

#include "app/session.h"
#include "telemetry/phantom.h"
#include "telemetry/phantom_buffer.h"

namespace zg::app {

// The impure half of click-to-pin, shared by the mouse path (handle_pick)
// and the CLI /pin command so the two can never drift on side-effects or
// event logging. promote_phantom (pure, test_promote) computes the node +
// edges; this applies them: stored_nodes/edges push, incremental DB
// writes, physics enqueues, phantom_buffer removal, selection, spawn-
// tracker erase, and the phantom_pin event with time_to_pin_s.
//
// Physics may be null (tests) — the enqueues are skipped and positions
// catch up via load. session.db must be non-null; callers gate on it.
// Returns the new node's id.
long long pin_phantom(Session& s,
                      const zg::telemetry::Phantom& ph,
                      zg::telemetry::PhantomBuffer& phantom_buffer,
                      std::unordered_map<long long, double>& seen_phantom_spawn);

}  // namespace zg::app
