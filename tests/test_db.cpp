#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "persistence/db.h"

#include <cmath>

using zg::persistence::Database;
using zg::persistence::StoredNode;
using zg::graph::Edge;

namespace {
bool near(float a, float b) { return std::fabs(a - b) < 1e-4f; }
}

TEST_CASE("db: schema is created on open; empty load returns false") {
    Database db(":memory:");
    std::vector<StoredNode> nodes;
    std::vector<Edge>       edges;
    CHECK_FALSE(db.load_graph(nodes, edges));
    CHECK(nodes.empty());
    CHECK(edges.empty());
}

TEST_CASE("db: roundtrip nodes + edges + markdown content") {
    Database db(":memory:");

    std::vector<StoredNode> in_nodes = {
        {1, {1.0f, 2.0f, 3.0f}, "alpha", "# heading\nbody"},
        {2, {4.5f, 5.5f, 6.5f}, "beta",  ""},
        {3, {0.0f, 0.0f, 0.0f}, "",      "lone content"},
    };
    std::vector<Edge> in_edges = {{1, 2}, {2, 3}, {1, 3}};

    db.save_graph(in_nodes, in_edges);

    std::vector<StoredNode> out_nodes;
    std::vector<Edge>       out_edges;
    REQUIRE(db.load_graph(out_nodes, out_edges));

    REQUIRE(out_nodes.size() == in_nodes.size());
    for (std::size_t i = 0; i < in_nodes.size(); ++i) {
        CHECK(out_nodes[i].id == in_nodes[i].id);
        CHECK(near(out_nodes[i].position.x, in_nodes[i].position.x));
        CHECK(near(out_nodes[i].position.y, in_nodes[i].position.y));
        CHECK(near(out_nodes[i].position.z, in_nodes[i].position.z));
        CHECK(out_nodes[i].title   == in_nodes[i].title);
        CHECK(out_nodes[i].content == in_nodes[i].content);
    }

    REQUIRE(out_edges.size() == in_edges.size());
    for (std::size_t i = 0; i < in_edges.size(); ++i) {
        CHECK(out_edges[i].source == in_edges[i].source);
        CHECK(out_edges[i].target == in_edges[i].target);
    }
}

TEST_CASE("db: save_graph replaces previous contents") {
    Database db(":memory:");

    db.save_graph({{1, {0,0,0}, "first", ""}}, {});
    db.save_graph({{42, {9, 9, 9}, "second", "body"}}, {{42, 42}});

    std::vector<StoredNode> nodes;
    std::vector<Edge>       edges;
    db.load_graph(nodes, edges);

    REQUIRE(nodes.size() == 1);
    CHECK(nodes[0].id == 42);
    CHECK(nodes[0].title == "second");
    REQUIRE(edges.size() == 1);
    CHECK(edges[0].source == 42);
    CHECK(edges[0].target == 42);
}
