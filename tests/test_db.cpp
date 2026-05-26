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

TEST_CASE("db: search returns empty for empty / non-alphanumeric input") {
    Database db(":memory:");
    db.save_graph({{1, {0,0,0}, "alpha", ""}}, {});
    CHECK(db.search("").empty());
    CHECK(db.search("   ").empty());
    CHECK(db.search("!!!").empty());
}

TEST_CASE("db: search matches titles and content with prefix semantics") {
    Database db(":memory:");
    db.save_graph({
        {1, {0,0,0}, "alpha node",     "investigate the supply chain"},
        {2, {0,0,0}, "beta target",    "no overlap here"},
        {3, {0,0,0}, "gamma vendor",   "supply route mapping"},
    }, {});

    SUBCASE("title prefix match") {
        const auto hits = db.search("alph");
        REQUIRE(hits.size() == 1);
        CHECK(hits[0] == 1);
    }

    SUBCASE("content match returns its node") {
        const auto hits = db.search("investigate");
        REQUIRE(hits.size() == 1);
        CHECK(hits[0] == 1);
    }

    SUBCASE("shared term across nodes returns both") {
        const auto hits = db.search("supply");
        REQUIRE(hits.size() == 2);
        // Order is FTS rank-driven; just assert membership.
        CHECK((hits[0] == 1 || hits[0] == 3));
        CHECK((hits[1] == 1 || hits[1] == 3));
        CHECK(hits[0] != hits[1]);
    }

    SUBCASE("no match returns empty") {
        CHECK(db.search("zzzzzz").empty());
    }
}

TEST_CASE("db: search index is rebuilt on save (no stale rows)") {
    Database db(":memory:");
    db.save_graph({{7, {0,0,0}, "alpha", ""}}, {});
    REQUIRE(db.search("alpha").size() == 1);

    // Replace the graph; old "alpha" should not survive.
    db.save_graph({{99, {0,0,0}, "omega", ""}}, {});
    CHECK(db.search("alpha").empty());
    REQUIRE(db.search("omega").size() == 1);
    CHECK(db.search("omega")[0] == 99);
}

TEST_CASE("db: save empty graph leaves the load returning false") {
    Database db(":memory:");
    db.save_graph({}, {});

    std::vector<StoredNode> nodes;
    std::vector<Edge>       edges;
    CHECK_FALSE(db.load_graph(nodes, edges));
    CHECK(nodes.empty());
    CHECK(edges.empty());
    CHECK(db.search("anything").empty());
}

TEST_CASE("db: titles and content with SQL-special characters roundtrip safely") {
    Database db(":memory:");
    const std::string tricky_title   = "it's; he said \"hi\"";
    const std::string tricky_content = "'; DROP TABLE nodes; -- attempt";
    db.save_graph({{1, {0,0,0}, tricky_title, tricky_content}}, {});

    std::vector<StoredNode> nodes;
    std::vector<Edge>       edges;
    REQUIRE(db.load_graph(nodes, edges));
    REQUIRE(nodes.size() == 1);
    CHECK(nodes[0].title   == tricky_title);
    CHECK(nodes[0].content == tricky_content);
}

TEST_CASE("db: unicode survives roundtrip and is searchable by ascii prefix") {
    Database db(":memory:");
    db.save_graph({{1, {0,0,0}, "café αβγ 日本語", "espresso"}}, {});

    std::vector<StoredNode> nodes;
    std::vector<Edge>       edges;
    REQUIRE(db.load_graph(nodes, edges));
    CHECK(nodes[0].title   == "café αβγ 日本語");
    CHECK(nodes[0].content == "espresso");

    // FTS5 default tokenizer is unicode61; ascii prefix still hits the row.
    CHECK(db.search("espresso").size() == 1);
}

TEST_CASE("db: search is case-insensitive") {
    Database db(":memory:");
    db.save_graph({{1, {0,0,0}, "Alpha", ""}}, {});

    CHECK(db.search("alpha").size() == 1);
    CHECK(db.search("ALPHA").size() == 1);
    CHECK(db.search("AlPhA").size() == 1);
}

TEST_CASE("db: multi-token search has AND semantics") {
    Database db(":memory:");
    db.save_graph({
        {1, {0,0,0}, "supply chain analysis",       ""},
        {2, {0,0,0}, "supply node only",            ""},
        {3, {0,0,0}, "chain of evidence elsewhere", ""},
    }, {});

    const auto hits = db.search("supply chain");
    REQUIRE(hits.size() == 1);
    CHECK(hits[0] == 1);
}

TEST_CASE("db: load returns nodes ordered by id") {
    Database db(":memory:");
    db.save_graph({
        {7, {0,0,0}, "seven", ""},
        {2, {0,0,0}, "two",   ""},
        {5, {0,0,0}, "five",  ""},
    }, {});

    std::vector<StoredNode> nodes;
    std::vector<Edge>       edges;
    REQUIRE(db.load_graph(nodes, edges));
    REQUIRE(nodes.size() == 3);
    CHECK(nodes[0].id == 2);
    CHECK(nodes[1].id == 5);
    CHECK(nodes[2].id == 7);
}
