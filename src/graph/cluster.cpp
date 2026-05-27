#include "graph/cluster.h"

#include <algorithm>
#include <unordered_map>

namespace zg::graph {

std::vector<std::size_t> label_propagation(
    std::size_t node_count,
    const std::vector<Edge>& edges,
    int max_iters) {
    std::vector<std::size_t> labels(node_count);
    for (std::size_t i = 0; i < node_count; ++i) labels[i] = i;

    if (node_count == 0) return labels;

    // Build adjacency lists (undirected, no self-loops, in-bounds only).
    std::vector<std::vector<std::size_t>> adj(node_count);
    for (const Edge& e : edges) {
        if (e.source >= node_count || e.target >= node_count) continue;
        if (e.source == e.target) continue;
        adj[e.source].push_back(e.target);
        adj[e.target].push_back(e.source);
    }

    for (int iter = 0; iter < max_iters; ++iter) {
        bool changed = false;
        for (std::size_t i = 0; i < node_count; ++i) {
            if (adj[i].empty()) continue;  // isolated nodes keep their label

            // Count label occurrences among neighbors. Tie-break by the
            // smallest label so the algorithm is deterministic across runs.
            std::unordered_map<std::size_t, int> counts;
            for (std::size_t n : adj[i]) ++counts[labels[n]];

            std::size_t best_label = labels[i];
            int         best_count = 0;
            for (const auto& [lab, cnt] : counts) {
                if (cnt > best_count ||
                    (cnt == best_count && lab < best_label)) {
                    best_count = cnt;
                    best_label = lab;
                }
            }
            if (best_label != labels[i]) {
                labels[i] = best_label;
                changed   = true;
            }
        }
        if (!changed) break;
    }
    return labels;
}

}  // namespace zg::graph
