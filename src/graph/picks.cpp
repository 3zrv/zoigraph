#include "graph/picks.h"

#include <unordered_set>

namespace zg::graph {

namespace {

bool adjacent(std::size_t a, std::size_t b, const std::vector<Edge>& edges) {
    for (const Edge& e : edges) {
        if ((e.source == a && e.target == b) ||
            (e.source == b && e.target == a)) {
            return true;
        }
    }
    return false;
}

}  // namespace

std::vector<std::size_t> pick_weakly_connected_triple(
    std::size_t node_count,
    const std::vector<Edge>& edges,
    std::mt19937& rng,
    const std::vector<char>& alive) {
    if (node_count < 3) return {};

    // Pick only from the alive indices. With no mask this is exactly
    // [0, node_count), so the draw pattern (and every existing test) is
    // unchanged; with a mask, deleted nodes can never be chosen and the
    // fallback can't spin forever drawing them.
    const auto is_alive = [&alive](std::size_t i) {
        return alive.empty() || (i < alive.size() && alive[i]);
    };
    std::vector<std::size_t> live;
    live.reserve(node_count);
    for (std::size_t i = 0; i < node_count; ++i) {
        if (is_alive(i)) live.push_back(i);
    }
    if (live.size() < 3) return {};

    std::uniform_int_distribution<std::size_t> dist(0, live.size() - 1);

    // Try a bounded number of random triples to find one where no pair is
    // directly connected. 1000 attempts is plenty even for fairly dense
    // graphs of this scale.
    for (int attempt = 0; attempt < 1000; ++attempt) {
        const std::size_t a = live[dist(rng)];
        const std::size_t b = live[dist(rng)];
        const std::size_t c = live[dist(rng)];
        if (a == b || a == c || b == c) continue;
        if (adjacent(a, b, edges) || adjacent(a, c, edges) || adjacent(b, c, edges)) continue;
        return {a, b, c};
    }

    // Fully connected (or close to it): fall back to any three distinct nodes.
    std::unordered_set<std::size_t> picked;
    while (picked.size() < 3) picked.insert(live[dist(rng)]);
    return {picked.begin(), picked.end()};
}

}  // namespace zg::graph
