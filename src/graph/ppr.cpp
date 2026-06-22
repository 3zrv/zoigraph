#include "graph/ppr.h"

#include <algorithm>

namespace zg::graph {

namespace {

// alive[i] with the "empty means everything is alive" convention.
inline bool is_alive(const std::vector<char>& alive, std::size_t i) {
    return alive.empty() || (i < alive.size() && alive[i]);
}

}  // namespace

std::vector<float> personalized_pagerank(
    std::size_t node_count,
    const std::vector<Edge>& edges,
    std::size_t anchor,
    const std::vector<char>& alive,
    float restart,
    int iterations) {
    std::vector<float> rank(node_count, 0.0f);
    if (node_count == 0 || anchor >= node_count) return rank;
    if (!is_alive(alive, anchor)) return rank;

    // Undirected adjacency: alive-masked, no self-loops, in-bounds only.
    // Parallel edges keep their multiplicity — a doubled edge is a stronger
    // link, and deduping is a job for the data, not the relevance metric.
    std::vector<std::vector<std::size_t>> adj(node_count);
    for (const Edge& e : edges) {
        if (e.source >= node_count || e.target >= node_count) continue;
        if (e.source == e.target) continue;
        if (!is_alive(alive, e.source) || !is_alive(alive, e.target)) continue;
        adj[e.source].push_back(e.target);
        adj[e.target].push_back(e.source);
    }

    // 0 (never teleports) and 1 (never walks) are degenerate; clamp into range.
    if (restart < 0.0f) restart = 0.0f;
    if (restart > 1.0f) restart = 1.0f;
    const float walk = 1.0f - restart;

    rank[anchor] = 1.0f;  // all initial mass sits on the anchor
    std::vector<float> next(node_count, 0.0f);

    for (int it = 0; it < iterations; ++it) {
        std::fill(next.begin(), next.end(), 0.0f);
        next[anchor] += restart;  // the teleport share returns to the anchor

        for (std::size_t u = 0; u < node_count; ++u) {
            if (rank[u] == 0.0f) continue;
            if (adj[u].empty()) {
                next[anchor] += walk * rank[u];  // dangling node → back to anchor
                continue;
            }
            const float share = walk * rank[u] / static_cast<float>(adj[u].size());
            for (std::size_t v : adj[u]) next[v] += share;
        }
        rank.swap(next);
    }
    return rank;
}

std::vector<std::size_t> top_related(
    std::size_t node_count,
    const std::vector<Edge>& edges,
    std::size_t anchor,
    std::size_t k,
    const std::vector<char>& alive,
    float restart,
    int iterations) {
    const std::vector<float> rank =
        personalized_pagerank(node_count, edges, anchor, alive, restart, iterations);

    std::vector<std::size_t> cand;
    cand.reserve(node_count);
    for (std::size_t i = 0; i < node_count; ++i) {
        if (i == anchor) continue;
        if (!is_alive(alive, i)) continue;
        if (rank[i] <= 0.0f) continue;  // unreachable from the anchor
        cand.push_back(i);
    }
    std::sort(cand.begin(), cand.end(),
        [&rank](std::size_t a, std::size_t b) {
            if (rank[a] != rank[b]) return rank[a] > rank[b];
            return a < b;  // deterministic tie-break by index
        });
    if (cand.size() > k) cand.resize(k);
    return cand;
}

}  // namespace zg::graph
