#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "graph/ppr.h"

#include <algorithm>
#include <numeric>

using zg::graph::Edge;
using zg::graph::personalized_pagerank;
using zg::graph::top_related;

namespace {
float sum(const std::vector<float>& v) {
    return std::accumulate(v.begin(), v.end(), 0.0f);
}
}  // namespace

TEST_CASE("ppr: empty graph and out-of-range anchor yield empty / zero") {
    CHECK(personalized_pagerank(0, {}, 0).empty());
    CHECK(top_related(0, {}, 0, 5).empty());

    // anchor past the end → all-zero scores, no candidates.
    auto rank = personalized_pagerank(3, {{0, 1}}, 9);
    REQUIRE(rank.size() == 3);
    CHECK(rank[0] == 0.0f);
    CHECK(rank[1] == 0.0f);
    CHECK(rank[2] == 0.0f);
    CHECK(top_related(3, {{0, 1}}, 9, 5).empty());
}

TEST_CASE("ppr: isolated anchor keeps all its mass, has no relatives") {
    auto rank = personalized_pagerank(3, {{1, 2}}, 0);  // 0 is isolated
    CHECK(rank[0] == doctest::Approx(1.0f));
    CHECK(rank[1] == 0.0f);
    CHECK(rank[2] == 0.0f);
    CHECK(top_related(3, {{1, 2}}, 0, 5).empty());
}

TEST_CASE("ppr: mass is conserved and the anchor outranks far-off nodes") {
    // 0-1-2-3 path, anchored at one end.
    std::vector<Edge> path = {{0, 1}, {1, 2}, {2, 3}};
    auto rank = personalized_pagerank(4, path, 0);

    CHECK(sum(rank) == doctest::Approx(1.0f).epsilon(0.01));
    CHECK(rank[0] > 0.0f);      // the anchor retains weight every step
    CHECK(rank[0] > rank[2]);   // and outranks nodes two-plus hops away
    CHECK(rank[0] > rank[3]);
    // (The degree-2 neighbour 1 may outscore the degree-1 anchor — expected
    // for an undirected walk; that is why top_related excludes the anchor.)
}

TEST_CASE("ppr: relevance decays with graph distance from the anchor") {
    std::vector<Edge> path = {{0, 1}, {1, 2}, {2, 3}};
    auto rank = personalized_pagerank(4, path, 0);

    CHECK(rank[1] > rank[2]);
    CHECK(rank[2] > rank[3]);
    // Direction is ignored — the same path anchored at 3 mirrors it.
    auto mirror = personalized_pagerank(4, path, 3);
    CHECK(mirror[2] > mirror[1]);
    CHECK(mirror[1] > mirror[0]);

    CHECK(top_related(4, path, 0, 2) == std::vector<std::size_t>{1, 2});
}

TEST_CASE("ppr: a symmetric star ranks every leaf equally, tie-broken by index") {
    // center 0 linked to leaves 1..4.
    std::vector<Edge> star = {{0, 1}, {0, 2}, {0, 3}, {0, 4}};
    auto rank = personalized_pagerank(5, star, 0);

    CHECK(rank[1] == doctest::Approx(rank[2]));
    CHECK(rank[2] == doctest::Approx(rank[3]));
    CHECK(rank[3] == doctest::Approx(rank[4]));

    // All tied → the first two by index win deterministically.
    CHECK(top_related(5, star, 0, 2) == std::vector<std::size_t>{1, 2});
}

TEST_CASE("ppr: a dead node blocks flow and is never returned as a relative") {
    // 0-1-2-3 path with node 2 tombstoned: 3 becomes unreachable from 0.
    std::vector<Edge> path = {{0, 1}, {1, 2}, {2, 3}};
    std::vector<char> alive = {1, 1, 0, 1};
    auto rank = personalized_pagerank(4, path, 0, alive);

    CHECK(rank[1] > 0.0f);
    CHECK(rank[2] == 0.0f);  // dead: holds no mass
    CHECK(rank[3] == 0.0f);  // only path ran through the dead node

    // k larger than the candidate set: only the reachable, alive node 1.
    CHECK(top_related(4, path, 0, 10, alive) == std::vector<std::size_t>{1});
}

TEST_CASE("ppr: disconnected components are unreachable, excluded from relatives") {
    // {0-1} and {2-3} are separate components; anchor in the first.
    std::vector<Edge> two = {{0, 1}, {2, 3}};
    auto rank = personalized_pagerank(4, two, 0);

    CHECK(rank[1] > 0.0f);
    CHECK(rank[2] == 0.0f);
    CHECK(rank[3] == 0.0f);
    CHECK(top_related(4, two, 0, 10) == std::vector<std::size_t>{1});
}

TEST_CASE("ppr: self-loops and out-of-bounds / cross-dead edges are ignored") {
    // self-loop on the anchor, an edge to a dead node, an out-of-range edge.
    std::vector<Edge> edges = {{0, 0}, {0, 1}, {0, 2}, {0, 9}};
    std::vector<char> alive = {1, 1, 0};  // node 2 dead
    auto rank = personalized_pagerank(3, edges, 0, alive);

    // Only 0-1 survives masking: all walk-mass that leaves 0 lands on 1.
    CHECK(rank[1] > 0.0f);
    CHECK(rank[2] == 0.0f);
    CHECK(top_related(3, edges, 0, 10, alive) == std::vector<std::size_t>{1});
}
