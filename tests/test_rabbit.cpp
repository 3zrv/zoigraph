#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "macros/rabbit_hole.h"
#include "graph/types.h"

#include <algorithm>
#include <random>
#include <vector>

using zg::graph::Edge;
using zg::macros::pick_rabbit_path;

namespace {
bool edge_between(std::size_t a, std::size_t b, const std::vector<Edge>& edges) {
    for (const Edge& e : edges) {
        if ((e.source == a && e.target == b) || (e.source == b && e.target == a))
            return true;
    }
    return false;
}
bool contains(const std::vector<std::size_t>& v, std::size_t x) {
    return std::find(v.begin(), v.end(), x) != v.end();
}
}  // namespace

TEST_CASE("rabbit: path starts at the anchor and every hop is a real edge") {
    const std::vector<Edge> chain = {{0, 1}, {1, 2}, {2, 3}};
    std::mt19937 rng(5);
    for (int trial = 0; trial < 50; ++trial) {
        const auto path = pick_rabbit_path(0, chain, rng, 4);
        REQUIRE(!path.empty());
        CHECK(path.front() == 0u);
        CHECK(path.size() <= 4);  // start + at most kRabbitHopCount hops
        for (std::size_t i = 0; i + 1 < path.size(); ++i) {
            CHECK(edge_between(path[i], path[i + 1], chain));
        }
    }
}

TEST_CASE("rabbit: never hops to a tombstoned node") {
    // chain 0-1-2-3 with node 2 dead: hops can never reach 2 or (through it) 3.
    const std::vector<Edge> chain = {{0, 1}, {1, 2}, {2, 3}};
    const std::vector<char> alive = {1, 1, 0, 1};
    for (unsigned seed = 0; seed < 20; ++seed) {
        std::mt19937 rng(seed);
        const auto path = pick_rabbit_path(0, chain, rng, 4, alive);
        CHECK(path.front() == 0u);
        CHECK_FALSE(contains(path, 2u));
        CHECK_FALSE(contains(path, 3u));
    }
}

TEST_CASE("rabbit: a node whose only neighbour is dead yields just the start") {
    const std::vector<Edge> edges = {{0, 1}};
    const std::vector<char> alive = {1, 0};  // node 1 dead
    std::mt19937 rng(2);
    const auto path = pick_rabbit_path(0, edges, rng, 2, alive);
    REQUIRE(path.size() == 1);
    CHECK(path[0] == 0u);
}

TEST_CASE("rabbit: an empty alive mask behaves exactly like no mask") {
    const std::vector<Edge> chain = {{0, 1}, {1, 2}, {2, 3}};
    std::mt19937 rng_a(11), rng_b(11);
    const auto a = pick_rabbit_path(0, chain, rng_a, 4);
    const auto b = pick_rabbit_path(0, chain, rng_b, 4, {});
    CHECK(a == b);
}
