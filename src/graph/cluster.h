#pragma once

#include <cstddef>
#include <vector>

#include "graph/types.h"

namespace zg::graph {

// Label-propagation community detection over an undirected graph.
//
// Each node starts with a unique label (its own index). At each pass, every
// node updates its label to the most-common label among its neighbors (ties
// broken by the smallest label). The pass repeats until either nothing
// changes or `max_iters` is hit.
//
// Returns one cluster id per node, parallel to indices 0..node_count-1.
// Two nodes end up with the same cluster id iff label propagation merged
// them into the same community. Isolated nodes keep their own initial id.
//
// Edge direction is ignored. Self-loops are ignored. Edges with endpoint
// indices >= node_count are silently dropped.
std::vector<std::size_t> label_propagation(
    std::size_t node_count,
    const std::vector<Edge>& edges,
    int max_iters = 50);

}  // namespace zg::graph
