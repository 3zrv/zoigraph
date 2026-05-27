#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "persistence/seed.h"

#include <cmath>
#include <set>
#include <string>

using zg::persistence::InitialGraph;
using zg::persistence::make_initial_graph;
using zg::persistence::make_random_fill;

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

TEST_CASE("random_fill: requested counts are honored exactly") {
    const auto g = make_random_fill(/*nodes=*/300, /*edges=*/30,
                                    /*start_id=*/3, /*now=*/100.0);
    CHECK(g.nodes.size() == 300);
    CHECK(g.edges.size() == 30);
}

TEST_CASE("random_fill: ids start at start_id and are contiguous") {
    const auto g = make_random_fill(50, 0, /*start_id=*/3, 100.0);
    REQUIRE(g.nodes.size() == 50);
    for (std::size_t i = 0; i < g.nodes.size(); ++i) {
        CHECK(g.nodes[i].id == static_cast<long long>(3 + i));
    }
}

TEST_CASE("random_fill: edges reference only ids in the new range") {
    const auto g = make_random_fill(50, 100, /*start_id=*/3, 100.0);
    REQUIRE(g.nodes.size() == 50);
    for (const auto& e : g.edges) {
        CHECK(e.source >= 3);
        CHECK(e.target >= 3);
        CHECK(e.source <  3 + 50);
        CHECK(e.target <  3 + 50);
        CHECK(e.source != e.target);
    }
}

TEST_CASE("random_fill: every node carries the supplied timestamp") {
    const auto g = make_random_fill(20, 0, 0, /*now=*/1234.5);
    REQUIRE(g.nodes.size() == 20);
    for (const auto& n : g.nodes) {
        CHECK(n.first_seen   == doctest::Approx(1234.5));
        CHECK(n.last_touched == doctest::Approx(1234.5));
        CHECK(n.tier == "confirmed");
        CHECK(n.title.empty());
    }
}

TEST_CASE("random_fill: same seed produces identical output") {
    const auto a = make_random_fill(50, 30, 0, 0.0, 20.0f, /*rng=*/7);
    const auto b = make_random_fill(50, 30, 0, 0.0, 20.0f, /*rng=*/7);
    REQUIRE(a.nodes.size() == b.nodes.size());
    REQUIRE(a.edges.size() == b.edges.size());
    for (std::size_t i = 0; i < a.nodes.size(); ++i) {
        CHECK(a.nodes[i].position.x == doctest::Approx(b.nodes[i].position.x));
    }
}

TEST_CASE("random_fill: node_count = 0 returns empty graph") {
    const auto g = make_random_fill(0, 0, 0, 0.0);
    CHECK(g.nodes.empty());
    CHECK(g.edges.empty());
}

TEST_CASE("random_fill: edge_count > 0 with node_count <= 1 yields no edges") {
    const auto g = make_random_fill(1, 50, 0, 0.0);
    REQUIRE(g.nodes.size() == 1);
    CHECK(g.edges.empty());
}

TEST_CASE("random_fill: with_data=false yields empty titles/content/tags") {
    const auto g = make_random_fill(50, 0, 0, 100.0, 25.0f, 42, /*with_data=*/false);
    REQUIRE(g.nodes.size() == 50);
    for (const auto& n : g.nodes) {
        CHECK(n.title.empty());
        CHECK(n.content.empty());
        CHECK(n.tags.empty());
    }
}

TEST_CASE("random_fill: with_data=true populates title, content, and some tags") {
    const auto g = make_random_fill(300, 0, 3, 100.0, 25.0f, 42, /*with_data=*/true);
    REQUIRE(g.nodes.size() == 300);
    int empty_title  = 0;
    int empty_content = 0;
    int with_tags    = 0;
    for (const auto& n : g.nodes) {
        if (n.title.empty())   ++empty_title;
        if (n.content.empty()) ++empty_content;
        if (!n.tags.empty())   ++with_tags;
    }
    CHECK(empty_title == 0);
    CHECK(empty_content == 0);
    // With tag_count uniform [0, 3], roughly 75% of nodes get >= 1 tag.
    // Use a conservative lower bound so the test isn't flaky.
    CHECK(with_tags > 150);
}

TEST_CASE("random_fill: with_data tags come from the known pool only") {
    const auto g = make_random_fill(100, 0, 0, 100.0, 25.0f, 42, /*with_data=*/true);
    const std::set<std::string> pool = {
        "subject", "asset", "front", "hostile",
        "deceased", "informant", "surveillance", "lead",
    };
    for (const auto& n : g.nodes) {
        for (const auto& t : n.tags) {
            CHECK(pool.count(t) == 1);
        }
    }
}

TEST_CASE("random_fill: with_data title contains the node id (so titles are unique)") {
    const auto g = make_random_fill(100, 0, /*start_id=*/3, 100.0, 25.0f, 42, true);
    std::set<std::string> seen_titles;
    for (const auto& n : g.nodes) {
        CHECK(seen_titles.insert(n.title).second);  // returns false if already there
    }
}

TEST_CASE("random_fill: with_data per-node tags don't contain duplicates") {
    const auto g = make_random_fill(300, 0, 0, 100.0, 25.0f, 42, true);
    for (const auto& n : g.nodes) {
        std::set<std::string> unique(n.tags.begin(), n.tags.end());
        CHECK(unique.size() == n.tags.size());
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
