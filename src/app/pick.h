#pragma once

#include <raylib.h>

#include <unordered_map>
#include <vector>

#include "app/session.h"
#include "telemetry/phantom.h"
#include "telemetry/phantom_buffer.h"

namespace zg::app {

// Persistent state for the double-click-to-inspect detector. One instance
// per main(); reset on detection so a fast triple-click doesn't fire two
// inspector opens in a row.
struct DoubleClickState {
    double last_t   = -100.0;
    int    last_idx = -1;
};

// Handles a single frame of left-mouse picking. No-op when ImGui already
// wants the mouse (panel hover / drag). Phantoms get first crack — a hit
// on one promotes it to a Static Node (writes to DB, queues into physics,
// removes from phantom_buffer, materializes any 'connections' as real
// edges). If no phantom is hit, picks the closest static node by ray
// intersection. A second click on the same node within 350 ms sets
// focus_inspector so the next ImGui frame can flip to the Inspector tab.
//
// `seen_phantom_spawn` is the phase-2 spawn-tracker map (phantom id ->
// spawn unix-ts). On a successful pin we erase the picked phantom's
// entry so main's per-frame diff doesn't misclassify the pin as a decay,
// and log a `phantom_pin` event into the session DB with the time-to-pin.
//
// Mutates session.{stored_nodes, edges, selected_node}, the
// phantom_buffer, the dbl tracker, focus_inspector, and
// seen_phantom_spawn.
void handle_pick(Session& session,
                 const Camera3D& camera,
                 const std::vector<zg::telemetry::Phantom>& phantoms,
                 zg::telemetry::PhantomBuffer& phantom_buffer,
                 DoubleClickState& dbl,
                 bool& focus_inspector,
                 std::unordered_map<long long, double>& seen_phantom_spawn);

}  // namespace zg::app
