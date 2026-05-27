#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "persistence/seed.h"

#include <cmath>

using zg::persistence::InitialGraph;
using zg::persistence::make_initial_graph;

TEST_CASE("seed: returns exactly three nodes and no edges") {
    const auto g = make_initial_graph(100.0);
    CHECK(g.nodes.size() == 3);
    CHECK(g.edges.empty());
}

TEST_CASE("seed: node 0 is `self` at the origin in tier 'self'") {
    const auto g = make_initial_graph(100.0);
    REQUIRE(g.nodes.size() >= 1);
    CHECK(g.nodes[0].id == 0);
    CHECK(g.nodes[0].title == "self");
    CHECK(g.nodes[0].tier == "self");
    CHECK(g.nodes[0].position.x == doctest::Approx(0.0f));
    CHECK(g.nodes[0].position.y == doctest::Approx(0.0f));
    CHECK(g.nodes[0].position.z == doctest::Approx(0.0f));
}

TEST_CASE("seed: nodes 1 and 2 are alice and bob, confirmed, on the x axis") {
    const auto g = make_initial_graph(100.0);
    REQUIRE(g.nodes.size() == 3);
    CHECK(g.nodes[1].id == 1);
    CHECK(g.nodes[1].title == "alice");
    CHECK(g.nodes[1].tier == "confirmed");
    CHECK(g.nodes[1].position.y == doctest::Approx(0.0f));
    CHECK(g.nodes[1].position.z == doctest::Approx(0.0f));
    CHECK(g.nodes[2].id == 2);
    CHECK(g.nodes[2].title == "bob");
    CHECK(g.nodes[2].tier == "confirmed");
    CHECK(g.nodes[2].position.y == doctest::Approx(0.0f));
    CHECK(g.nodes[2].position.z == doctest::Approx(0.0f));
    // alice on the left, bob on the right
    CHECK(g.nodes[1].position.x < g.nodes[2].position.x);
}

TEST_CASE("seed: all three nodes share the same first_seen and last_touched") {
    const auto g = make_initial_graph(1234.5);
    REQUIRE(g.nodes.size() == 3);
    for (const auto& n : g.nodes) {
        CHECK(n.first_seen   == doctest::Approx(1234.5));
        CHECK(n.last_touched == doctest::Approx(1234.5));
    }
}

TEST_CASE("seed: node ids are 0, 1, 2 contiguous") {
    const auto g = make_initial_graph(0.0);
    REQUIRE(g.nodes.size() == 3);
    CHECK(g.nodes[0].id == 0);
    CHECK(g.nodes[1].id == 1);
    CHECK(g.nodes[2].id == 2);
}

TEST_CASE("seed: content fields are sensible defaults") {
    const auto g = make_initial_graph(0.0);
    REQUIRE(g.nodes.size() == 3);
    // self has a short description so the operator sees something on click.
    CHECK_FALSE(g.nodes[0].content.empty());
    // alice and bob start blank — the operator fills them in.
    CHECK(g.nodes[1].content.empty());
    CHECK(g.nodes[2].content.empty());
}

TEST_CASE("seed: same now_unix twice produces identical graphs") {
    const auto a = make_initial_graph(7777.0);
    const auto b = make_initial_graph(7777.0);
    REQUIRE(a.nodes.size() == b.nodes.size());
    for (std::size_t i = 0; i < a.nodes.size(); ++i) {
        CHECK(a.nodes[i].id == b.nodes[i].id);
        CHECK(a.nodes[i].title == b.nodes[i].title);
        CHECK(a.nodes[i].first_seen   == doctest::Approx(b.nodes[i].first_seen));
        CHECK(a.nodes[i].last_touched == doctest::Approx(b.nodes[i].last_touched));
    }
}

TEST_CASE("seed: positions are distinct (nodes don't pile at origin)") {
    const auto g = make_initial_graph(0.0);
    REQUIRE(g.nodes.size() == 3);
    auto same = [](Vector3 a, Vector3 b) {
        return std::fabs(a.x - b.x) < 1e-4f &&
               std::fabs(a.y - b.y) < 1e-4f &&
               std::fabs(a.z - b.z) < 1e-4f;
    };
    CHECK_FALSE(same(g.nodes[0].position, g.nodes[1].position));
    CHECK_FALSE(same(g.nodes[0].position, g.nodes[2].position));
    CHECK_FALSE(same(g.nodes[1].position, g.nodes[2].position));
}
