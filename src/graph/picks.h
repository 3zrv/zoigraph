#pragma once

#include <cstddef>
#include <random>
#include <vector>

#include "graph/types.h"

namespace zg::graph {

// Picks three distinct node indices in [0, node_count) such that, where
// possible, no pair among them shares a direct edge. "Weakly connected" in
// the throw-the-bones sense: we want the operator to confront combinations
// the graph doesn't already suggest.
//
// If the graph is dense enough that no such triple exists within a bounded
// search, falls back to any three distinct indices. Returns empty if
// node_count < 3.
//
// `alive` (empty == all alive) excludes tombstoned nodes: a deleted node is
// never one of the three. Returns empty if fewer than 3 nodes are alive.
std::vector<std::size_t> pick_weakly_connected_triple(
    std::size_t node_count,
    const std::vector<Edge>& edges,
    std::mt19937& rng,
    const std::vector<char>& alive = {});

}  // namespace zg::graph
