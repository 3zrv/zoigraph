#pragma once

#include <cstddef>
#include <vector>

#include "graph/types.h"

namespace zg::graph {

// Personalized PageRank restarted at `anchor`, over the *undirected* graph
// (an edge a–b makes a and b mutually relevant; direction is ignored). Power
// iteration on the row-normalized walk matrix with teleport-to-anchor
// probability `restart` (the standard 0.15 ≈ 1 − 0.85 damping): each step,
// `restart` of the mass jumps back to the anchor and the rest flows along
// edges. Converges fast — ~20 iterations is plenty at this graph's scale.
//
// `alive` (empty == all alive) masks tombstoned nodes: a dead node neither
// holds nor propagates score, and edges touching one are dropped. The anchor
// must be alive and in-bounds, else an all-zero vector comes back. Dangling
// mass (alive nodes with no alive neighbour) teleports back to the anchor, so
// the returned scores sum to ~1 over the alive set.
//
// Returns one score per node index 0..node_count-1. Scores are > 0 for nodes
// reachable from the anchor (the anchor itself included) and exactly 0
// otherwise; relevance falls off with graph distance. The anchor is *not*
// guaranteed to be the global maximum — in an undirected walk a high-degree
// near neighbour can outscore a low-degree anchor — but `top_related` excludes
// the anchor regardless, so consumers rank only the surrounding nodes.
std::vector<float> personalized_pagerank(
    std::size_t node_count,
    const std::vector<Edge>& edges,
    std::size_t anchor,
    const std::vector<char>& alive = {},
    float restart = 0.15f,
    int iterations = 20);

// The top-`k` node indices most relevant to `anchor` by personalized
// PageRank, excluding the anchor itself, any dead node, and any node with a
// zero score (unreachable). Descending score; ties broken by ascending index
// for determinism. Drives ask-context selection and the query channel's
// neighbourhood fan-out — graph relevance instead of raw spatial proximity.
std::vector<std::size_t> top_related(
    std::size_t node_count,
    const std::vector<Edge>& edges,
    std::size_t anchor,
    std::size_t k,
    const std::vector<char>& alive = {},
    float restart = 0.15f,
    int iterations = 20);

}  // namespace zg::graph
