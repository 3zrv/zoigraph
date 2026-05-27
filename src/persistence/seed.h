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
//
// The picker is deliberately tiny: a new project is meant to be a blank
// canvas with just the operator and two named placeholders to start
// building edges against. The bulk-random-graph from earlier prototypes is
// gone.
InitialGraph make_initial_graph(double now_unix);

}  // namespace zg::persistence
