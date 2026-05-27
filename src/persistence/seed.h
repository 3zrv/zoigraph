#pragma once

#include <vector>

#include "graph/types.h"
#include "persistence/db.h"

namespace zg::persistence {

struct InitialGraph {
    std::vector<StoredNode>      nodes;
    std::vector<graph::Edge>     edges;
};

// Returns the seed graph for a freshly-created project: three Static Nodes —
// `self` at the origin (tier "self"), `alice` to the left, `bob` to the right
// — with no edges between them. first_seen and last_touched on every node
// are set to `now_unix`.
InitialGraph make_initial_graph(double now_unix);

// Bulk-fill: `node_count` untitled "confirmed"-tier nodes scattered in a
// cube of half-extent `spread`, plus `edge_count` random edges among them.
// Ids start at `start_id` (typically the size of the named seed graph
// you're appending onto). All timestamps are `now_unix`. `rng_seed` makes
// the output reproducible.
//
// Edges connect random distinct nodes within the new range only — they
// never reference ids < start_id, so an existing self/alice/bob seed stays
// untouched.
InitialGraph make_random_fill(int          node_count,
                              int          edge_count,
                              long long    start_id,
                              double       now_unix,
                              float        spread     = 25.0f,
                              unsigned int rng_seed   = 42);

}  // namespace zg::persistence
