#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "persistence/db.h"

#include <sqlite3.h>

#include <cmath>
#include <cstdio>
#include <string>
#include <sys/types.h>
#include <unistd.h>

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

TEST_CASE("db: update_node_text updates the row and keeps FTS in sync") {
    Database db(":memory:");
    db.save_graph({{1, {0,0,0}, "old title", "old body"}}, {});

    REQUIRE(db.search("old").size() == 1);
    REQUIRE(db.search("new").empty());

    db.update_node_text(1, "new title", "fresh content", 0.0);

    CHECK(db.search("old").empty());
    REQUIRE(db.search("new").size() == 1);
    CHECK(db.search("new")[0] == 1);
    REQUIRE(db.search("fresh").size() == 1);
    REQUIRE(db.search("content").size() == 1);

    std::vector<StoredNode> nodes;
    std::vector<Edge>       edges;
    REQUIRE(db.load_graph(nodes, edges));
    CHECK(nodes[0].title   == "new title");
    CHECK(nodes[0].content == "fresh content");
}

TEST_CASE("db: persistence survives close + reopen against a real file") {
    const std::string path = "/tmp/zoigraph_test_" + std::to_string(::getpid()) + ".db";
    std::remove(path.c_str());
    std::remove((path + "-journal").c_str());

    {
        Database db(path);
        db.save_graph({
            {1, {1.0f, 2.0f, 3.0f}, "persistent",  "marker body"},
            {2, {4.0f, 5.0f, 6.0f}, "second-node", "more body"},
        }, {{1, 2}});
    }  // connection closes here

    {
        Database db(path);  // fresh connection against the same file
        std::vector<StoredNode> nodes;
        std::vector<Edge>       edges;
        REQUIRE(db.load_graph(nodes, edges));
        REQUIRE(nodes.size() == 2);
        CHECK(nodes[0].title   == "persistent");
        CHECK(nodes[0].content == "marker body");
        CHECK(nodes[1].title   == "second-node");
        REQUIRE(edges.size() == 1);
        CHECK(edges[0].source == 1);
        CHECK(edges[0].target == 2);

        // rebuild-on-open path: search should work without any save_graph in
        // this second connection.
        REQUIRE(db.search("persistent").size() == 1);
        CHECK(db.search("persistent")[0] == 1);
    }

    std::remove(path.c_str());
    std::remove((path + "-journal").c_str());
}

TEST_CASE("db: update_node_text on a non-existent id is a silent no-op") {
    Database db(":memory:");
    db.save_graph({{1, {0,0,0}, "real", ""}}, {});

    // Should not throw.
    db.update_node_text(9999, "ghost", "no row to update", 0.0);

    std::vector<StoredNode> nodes;
    std::vector<Edge>       edges;
    REQUIRE(db.load_graph(nodes, edges));
    REQUIRE(nodes.size() == 1);
    CHECK(nodes[0].id == 1);
    CHECK(nodes[0].title == "real");
    // The ghost write must not have created a phantom FTS row either.
    CHECK(db.search("ghost").empty());
}

TEST_CASE("db: update_node_text preserves the row's position fields") {
    Database db(":memory:");
    db.save_graph({{1, {1.5f, -2.5f, 7.0f}, "before", "body"}}, {});

    db.update_node_text(1, "after", "new body", 0.0);

    std::vector<StoredNode> nodes;
    std::vector<Edge>       edges;
    REQUIRE(db.load_graph(nodes, edges));
    REQUIRE(nodes.size() == 1);
    CHECK(nodes[0].title == "after");
    CHECK(nodes[0].content == "new body");
    CHECK(nodes[0].position.x == doctest::Approx(1.5f));
    CHECK(nodes[0].position.y == doctest::Approx(-2.5f));
    CHECK(nodes[0].position.z == doctest::Approx(7.0f));
}

TEST_CASE("db: edges referencing non-existent nodes are tolerated (no FK enforced)") {
    Database db(":memory:");
    // Foreign keys are not enforced — useful while node ids are still
    // shuffled around. Edges referencing missing ids should round-trip.
    db.save_graph({{1, {0,0,0}, "n", ""}}, {{1, 99}, {77, 88}});

    std::vector<StoredNode> nodes;
    std::vector<Edge>       edges;
    REQUIRE(db.load_graph(nodes, edges));
    CHECK(nodes.size() == 1);
    REQUIRE(edges.size() == 2);
    CHECK(edges[0].source == 1);
    CHECK(edges[0].target == 99);
    CHECK(edges[1].source == 77);
    CHECK(edges[1].target == 88);
}

TEST_CASE("db: an 8 KB content blob roundtrips exactly") {
    Database db(":memory:");
    std::string big;
    big.reserve(8192);
    for (int i = 0; i < 8192; ++i) {
        big.push_back(static_cast<char>('a' + (i % 26)));
    }
    db.save_graph({{1, {0,0,0}, "long-content", big}}, {});

    std::vector<StoredNode> nodes;
    std::vector<Edge>       edges;
    REQUIRE(db.load_graph(nodes, edges));
    REQUIRE(nodes.size() == 1);
    CHECK(nodes[0].content == big);
    CHECK(nodes[0].content.size() == 8192);
}

TEST_CASE("db: first_seen / last_touched / tier roundtrip through save_graph") {
    Database db(":memory:");
    StoredNode in{};
    in.id           = 7;
    in.position     = {1, 2, 3};
    in.title        = "node-with-tier";
    in.content      = "body";
    in.first_seen   = 1234567890.5;
    in.last_touched = 1234567899.25;
    in.tier         = "suspected";
    db.save_graph({in}, {});

    std::vector<StoredNode> out;
    std::vector<Edge>       e;
    REQUIRE(db.load_graph(out, e));
    REQUIRE(out.size() == 1);
    CHECK(out[0].first_seen   == doctest::Approx(1234567890.5));
    CHECK(out[0].last_touched == doctest::Approx(1234567899.25));
    CHECK(out[0].tier         == "suspected");
}

TEST_CASE("db: update_node_text bumps last_touched") {
    Database db(":memory:");
    StoredNode n{};
    n.id = 1;
    n.position = {0, 0, 0};
    n.title = "old";
    n.last_touched = 1.0;
    db.save_graph({n}, {});

    db.update_node_text(1, "new", "body", 999.5);

    std::vector<StoredNode> out;
    std::vector<Edge>       e;
    REQUIRE(db.load_graph(out, e));
    CHECK(out[0].title == "new");
    CHECK(out[0].last_touched == doctest::Approx(999.5));
}

TEST_CASE("db: update_node_tier changes only the tier") {
    Database db(":memory:");
    StoredNode n{};
    n.id = 1;
    n.position = {1, 2, 3};
    n.title = "stable";
    n.content = "body";
    n.first_seen = 100.0;
    n.last_touched = 200.0;
    n.tier = "confirmed";
    db.save_graph({n}, {});

    db.update_node_tier(1, "phantom");

    std::vector<StoredNode> out;
    std::vector<Edge>       e;
    REQUIRE(db.load_graph(out, e));
    CHECK(out[0].tier         == "phantom");
    CHECK(out[0].title        == "stable");
    CHECK(out[0].content      == "body");
    CHECK(out[0].first_seen   == doctest::Approx(100.0));
    CHECK(out[0].last_touched == doctest::Approx(200.0));
}

TEST_CASE("db: legacy table missing new columns is migrated on open") {
    const std::string path = "/tmp/zg_migrate_" + std::to_string(::getpid()) + ".db";
    std::remove(path.c_str());

    // Manually create a pre-migration DB shape using raw SQLite, then open
    // through Database and verify the ALTER ADD COLUMN path filled in the
    // new columns transparently.
    {
        sqlite3* raw = nullptr;
        REQUIRE(sqlite3_open(path.c_str(), &raw) == SQLITE_OK);
        sqlite3_exec(raw,
            "CREATE TABLE nodes ("
            "  id INTEGER PRIMARY KEY, x REAL, y REAL, z REAL,"
            "  title TEXT NOT NULL DEFAULT '', content TEXT NOT NULL DEFAULT '');"
            "CREATE TABLE edges (source INTEGER, target INTEGER, weight REAL);"
            "INSERT INTO nodes (id, x, y, z, title, content) VALUES (1, 0, 0, 0, 'legacy', 'old');",
            nullptr, nullptr, nullptr);
        sqlite3_close(raw);
    }

    Database db(path);
    std::vector<StoredNode> out;
    std::vector<Edge>       e;
    REQUIRE(db.load_graph(out, e));
    REQUIRE(out.size() == 1);
    CHECK(out[0].title        == "legacy");
    CHECK(out[0].first_seen   == doctest::Approx(0.0));
    CHECK(out[0].last_touched == doctest::Approx(0.0));
    CHECK(out[0].tier         == "confirmed");

    std::remove(path.c_str());
}

TEST_CASE("db: search query containing punctuation sanitizes safely") {
    // The parser strips anything non-alphanumeric, so "it's" becomes "it s"
    // → "it* s*". A naive concatenation into the FTS5 query string without
    // sanitization would explode on the apostrophe.
    Database db(":memory:");
    db.save_graph({{1, {0,0,0}, "it's complicated", ""}}, {});

    CHECK(db.search("it's").size() == 1);
    CHECK(db.search("complicated").size() == 1);
    CHECK(db.search("'; DROP TABLE nodes; --").empty());
}

TEST_CASE("db: repeated update_node_text final-write-wins") {
    Database db(":memory:");
    db.save_graph({{1, {0,0,0}, "v1", ""}}, {});

    db.update_node_text(1, "v2", "", 0.0);
    db.update_node_text(1, "v3", "", 0.0);
    db.update_node_text(1, "final", "settled body", 0.0);

    std::vector<StoredNode> nodes;
    std::vector<Edge>       edges;
    REQUIRE(db.load_graph(nodes, edges));
    CHECK(nodes[0].title   == "final");
    CHECK(nodes[0].content == "settled body");
    CHECK(db.search("v1").empty());
    CHECK(db.search("v2").empty());
    CHECK(db.search("final").size() == 1);
}

TEST_CASE("db: in-place edits survive a subsequent save_graph that preserves the row") {
    Database db(":memory:");
    db.save_graph({{1, {0,0,0}, "before", ""}}, {});
    db.update_node_text(1, "after", "edited", 0.0);

    // Simulate the main loop's save-on-shutdown path: re-save the entire
    // graph with the StoredNode struct carrying the edited fields.
    db.save_graph({{1, {1, 1, 1}, "after", "edited"}}, {});

    std::vector<StoredNode> nodes;
    std::vector<Edge>       edges;
    REQUIRE(db.load_graph(nodes, edges));
    CHECK(nodes[0].title   == "after");
    CHECK(nodes[0].content == "edited");
    CHECK(nodes[0].position.x == doctest::Approx(1.0f));
    CHECK(db.search("after").size() == 1);
}

TEST_CASE("db: update_node_text with empty title clears the previous FTS hit") {
    Database db(":memory:");
    db.save_graph({{1, {0,0,0}, "previously-titled", ""}}, {});
    REQUIRE(db.search("previously").size() == 1);

    db.update_node_text(1, "", "", 0.0);
    CHECK(db.search("previously").empty());

    std::vector<StoredNode> nodes;
    std::vector<Edge>       edges;
    REQUIRE(db.load_graph(nodes, edges));
    CHECK(nodes[0].title.empty());
    CHECK(nodes[0].content.empty());
}

TEST_CASE("db: save_graph with empty nodes leaves search returning nothing") {
    Database db(":memory:");
    db.save_graph({{1, {0,0,0}, "alpha", "body"}}, {});
    REQUIRE(db.search("alpha").size() == 1);

    // Wipe the graph by saving an empty one. FTS should also be empty.
    db.save_graph({}, {});
    CHECK(db.search("alpha").empty());
    CHECK(db.search("body").empty());

    std::vector<StoredNode> nodes;
    std::vector<Edge>       edges;
    CHECK_FALSE(db.load_graph(nodes, edges));
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
