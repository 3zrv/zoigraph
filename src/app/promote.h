#pragma once

#include <cstddef>
#include <vector>

#include "graph/types.h"
#include "persistence/db.h"
#include "telemetry/phantom.h"

namespace zg::app {

// Pure-logic outcome of click-to-pin: a StoredNode to push into the
// session's stored_nodes vector, plus the Edges materialised from the
// phantom's `connections` array. The render-loop integration (raypick,
// physics enqueue, DB write, event log) wraps this; the wrapping is
// Raylib-bound and therefore exempt from unit tests per directive §7.2.
struct PromotionResult {
    zg::persistence::StoredNode  node;
    std::vector<zg::graph::Edge> edges;
};

// Compute the StoredNode + Edges produced by promoting `ph` into the
// graph. `new_id` is the id the caller will assign (typically
// stored_nodes.size() at the call site). `now_ts` is the unix-seconds
// timestamp written into first_seen/last_touched. `positions_size` is
// the bound for connection-id range filtering -- any target id < 0,
// >= positions_size, or == new_id is silently dropped (skipped, not
// erroring) so the model can't materialise edges to nodes that don't
// exist or to itself.
//
// Promotion contract (the trust gradient on the wire):
//   - node.tier      = "phantom"      (lowest tier; operator promotes manually)
//   - edge.certainty = "phantom"      (lowest certainty; visible-but-faded)
//   - node.title     = ph.label       (the model's short label)
//   - node.content   = ph.content     (the model's reasoning, possibly empty)
//   - node.first_seen / last_touched  = now_ts
PromotionResult promote_phantom(const zg::telemetry::Phantom& ph,
                                long long new_id,
                                double now_ts,
                                std::size_t positions_size);

}  // namespace zg::app
