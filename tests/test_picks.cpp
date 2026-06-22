#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "graph/picks.h"
#include "graph/types.h"

#include <algorithm>
#include <random>

using zg::graph::Edge;
using zg::graph::pick_weakly_connected_triple;

namespace {

bool all_distinct(const std::vector<std::size_t>& v) {
    if (v.size() != 3) return false;
    return v[0] != v[1] && v[0] != v[2] && v[1] != v[2];
}

bool any_pair_adjacent(const std::vector<std::size_t>& v, const std::vector<Edge>& edges) {
    auto adj = [&](std::size_t a, std::size_t b) {
        for (const Edge& e : edges) {
            if ((e.source == a && e.target == b) ||
                (e.source == b && e.target == a)) return true;
        }
        return false;
    };
    return adj(v[0], v[1]) || adj(v[0], v[2]) || adj(v[1], v[2]);
}

}  // namespace

TEST_CASE("picks: fewer than 3 nodes returns empty") {
    std::mt19937 rng(0);
    CHECK(pick_weakly_connected_triple(0, {}, rng).empty());
    CHECK(pick_weakly_connected_triple(1, {}, rng).empty());
    CHECK(pick_weakly_connected_triple(2, {}, rng).empty());
}

TEST_CASE("picks: 3 nodes with no edges always returns all three (no adjacency to avoid)") {
    std::mt19937 rng(42);
    const auto pick = pick_weakly_connected_triple(3, {}, rng);
    REQUIRE(pick.size() == 3);
    CHECK(all_distinct(pick));
}

TEST_CASE("picks: avoids directly-adjacent pairs when the graph is sparse") {
    // 5 nodes with a single edge 0-1. Any triple that contains both 0 and 1
    // should be rejected; the picker should land on a triple that doesn't.
    std::vector<Edge> edges = {{0, 1}};
    std::mt19937 rng(1);

    bool ever_picked_01_pair = false;
    for (int trial = 0; trial < 50; ++trial) {
        const auto pick = pick_weakly_connected_triple(5, edges, rng);
        REQUIRE(pick.size() == 3);
        REQUIRE(all_distinct(pick));
        const bool has0 = std::find(pick.begin(), pick.end(), 0u) != pick.end();
        const bool has1 = std::find(pick.begin(), pick.end(), 1u) != pick.end();
        if (has0 && has1) ever_picked_01_pair = true;
    }
    CHECK_FALSE(ever_picked_01_pair);
}

TEST_CASE("picks: fully-connected graph still returns 3 distinct nodes (fallback)") {
    // K4: every pair is adjacent. There is no triple with all-non-adjacent
    // pairs; the picker should still return 3 distinct indices instead of
    // looping forever or returning empty.
    std::vector<Edge> edges = {
        {0, 1}, {0, 2}, {0, 3},
        {1, 2}, {1, 3},
        {2, 3},
    };
    std::mt19937 rng(7);
    const auto pick = pick_weakly_connected_triple(4, edges, rng);
    REQUIRE(pick.size() == 3);
    CHECK(all_distinct(pick));
}

TEST_CASE("picks: edge direction doesn't matter for adjacency") {
    // An edge stored {A, B} should make the pair (A, B) adjacent regardless
    // of the order the picker considers them. Verify by feeding the picker
    // a small graph where every undirected pair has exactly one stored
    // direction and asserting it never returns an adjacent triple.
    std::vector<Edge> edges = {{0, 1}, {2, 0}, {3, 1}};  // mixed source/target
    std::mt19937 rng(2025);
    for (int trial = 0; trial < 30; ++trial) {
        const auto pick = pick_weakly_connected_triple(5, edges, rng);
        REQUIRE(pick.size() == 3);
        // None of (pick[0],pick[1]), (pick[0],pick[2]), (pick[1],pick[2])
        // should equal any of the edges, in either direction.
        for (std::size_t i = 0; i < 3; ++i) {
            for (std::size_t j = i + 1; j < 3; ++j) {
                for (const Edge& e : edges) {
                    const bool match = (e.source == pick[i] && e.target == pick[j]) ||
                                       (e.source == pick[j] && e.target == pick[i]);
                    REQUIRE_FALSE(match);
                }
            }
        }
    }
}

TEST_CASE("picks: graph with a node count exactly equal to 3 always returns those 3") {
    std::vector<Edge> edges = {{0, 1}};  // one edge between 0 and 1
    std::mt19937 rng(0);
    // With only 3 nodes total, a valid triple is always (0,1,2) — the
    // picker will fall back to the K4-style branch when no non-adjacent
    // triple exists.
    for (int trial = 0; trial < 10; ++trial) {
        const auto pick = pick_weakly_connected_triple(3, edges, rng);
        REQUIRE(pick.size() == 3);
        CHECK(all_distinct(pick));
    }
}

TEST_CASE("picks: same seed produces the same triple (determinism)") {
    // Determinism over a fixed seed lets future features replay or test
    // bones throws against a known answer.
    std::vector<Edge> edges = {{0, 1}, {2, 3}};
    std::mt19937 rng_a(0xC0FFEE);
    std::mt19937 rng_b(0xC0FFEE);

    const auto a = pick_weakly_connected_triple(20, edges, rng_a);
    const auto b = pick_weakly_connected_triple(20, edges, rng_b);
    REQUIRE(a.size() == 3);
    REQUIRE(b.size() == 3);
    CHECK(a[0] == b[0]);
    CHECK(a[1] == b[1]);
    CHECK(a[2] == b[2]);
}

TEST_CASE("picks: result indices are always within [0, node_count)") {
    std::vector<Edge> edges = {{0, 5}, {2, 7}, {1, 3}};
    std::mt19937 rng(13);
    for (int trial = 0; trial < 100; ++trial) {
        const auto pick = pick_weakly_connected_triple(10, edges, rng);
        REQUIRE(pick.size() == 3);
        for (auto i : pick) CHECK(i < 10);
        // And: on a sparse graph of 10 nodes with 3 edges, the picker should
        // succeed with non-adjacent pairs the vast majority of the time. We
        // don't enforce 100% (theoretical fallback exists), but we sanity-
        // check the picker isn't accidentally returning adjacent triples.
        if (any_pair_adjacent(pick, edges)) {
            // If we ever hit the fallback on this sparse graph something's
            // off — the bound is 1000 attempts.
            FAIL("unexpected adjacent pair returned on a sparse graph");
        }
    }
}

TEST_CASE("picks: a tombstoned node is never one of the three") {
    // node 2 is deleted; with no edges every alive triple is valid, so the
    // picker has free rein — it must still never return node 2.
    const std::vector<char> alive = {1, 1, 0, 1, 1, 1};  // node 2 dead
    std::mt19937 rng(7);
    for (int trial = 0; trial < 300; ++trial) {
        const auto pick = pick_weakly_connected_triple(6, {}, rng, alive);
        REQUIRE(pick.size() == 3);
        CHECK(all_distinct(pick));
        for (auto i : pick) CHECK(i != 2u);
    }
}

TEST_CASE("picks: dense graph fallback also skips tombstoned nodes") {
    // Fully-connected 5-node graph forces the fallback path; node 0 dead.
    std::vector<Edge> edges;
    for (std::size_t i = 0; i < 5; ++i)
        for (std::size_t j = i + 1; j < 5; ++j) edges.push_back({i, j});
    const std::vector<char> alive = {0, 1, 1, 1, 1};  // node 0 dead
    std::mt19937 rng(3);
    for (int trial = 0; trial < 100; ++trial) {
        const auto pick = pick_weakly_connected_triple(5, edges, rng, alive);
        REQUIRE(pick.size() == 3);
        for (auto i : pick) CHECK(i != 0u);
    }
}

TEST_CASE("picks: fewer than 3 alive nodes returns empty") {
    const std::vector<char> alive = {1, 1, 0, 0, 0};  // only 2 alive
    std::mt19937 rng(1);
    CHECK(pick_weakly_connected_triple(5, {}, rng, alive).empty());
}

TEST_CASE("picks: an empty alive mask behaves exactly like no mask") {
    std::vector<Edge> edges = {{0, 5}, {2, 7}};
    std::mt19937 rng_a(99), rng_b(99);
    const auto a = pick_weakly_connected_triple(10, edges, rng_a);
    const auto b = pick_weakly_connected_triple(10, edges, rng_b, {});
    CHECK(a == b);
}
