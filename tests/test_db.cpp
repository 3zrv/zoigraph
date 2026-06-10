#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "persistence/db.h"

#include <sqlite3.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>

// Cross-platform PID for unique temp-file paths. _getpid on Windows and
// getpid on POSIX share the same signature; this keeps the existing
// "/tmp/zoigraph_test_<pid>.db" pattern compiling on the windows-latest
// CI runner without dragging in <unistd.h>.
#if defined(_WIN32)
    #include <process.h>
    #define ZG_GETPID() _getpid()
#else
    #include <unistd.h>
    #define ZG_GETPID() ::getpid()
#endif

using zg::persistence::Database;
using zg::persistence::StoredNode;
using zg::graph::Edge;

namespace {
bool near_eq(float a, float b) { return std::fabs(a - b) < 1e-4f; }

// Cross-platform temp DB path. Uses std::filesystem::temp_directory_path
// so it lands at /tmp on POSIX, %TEMP% on Windows, plus a PID + tag so
// parallel ctest invocations don't collide.
std::string tmp_db_path(const char* tag) {
    namespace fs = std::filesystem;
    return (fs::temp_directory_path()
            / ("zoigraph_test_" + std::string(tag) + "_"
               + std::to_string(ZG_GETPID()) + ".db")).string();
}
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
        CHECK(near_eq(out_nodes[i].position.x, in_nodes[i].position.x));
        CHECK(near_eq(out_nodes[i].position.y, in_nodes[i].position.y));
        CHECK(near_eq(out_nodes[i].position.z, in_nodes[i].position.z));
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

TEST_CASE("db: search excludes soft-deleted nodes (title and content)") {
    Database db(":memory:");
    StoredNode alive{};
    alive.id = 1; alive.title = "alpha visible";  alive.content = "shared keyword";
    StoredNode ghost{};
    ghost.id = 2; ghost.title = "spectral title"; ghost.content = "shared keyword";
    ghost.deleted = true;
    db.save_graph({alive, ghost}, {});

    // Content token present on both rows -- only the live one may surface.
    {
        const auto hits = db.search("shared");
        REQUIRE(hits.size() == 1);
        CHECK(hits[0] == 1);
    }

    // Title token unique to the tombstone -- must return nothing at all.
    CHECK(db.search("spectral").empty());

    // Tombstoning the remaining live node drops it out of search too.
    alive.deleted = true;
    db.save_graph({alive, ghost}, {});
    CHECK(db.search("shared").empty());
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
    const std::string path = tmp_db_path("default");
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
    const std::string path = tmp_db_path("migrate");
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

TEST_CASE("db: update_node_tier on a non-existent id is a silent no-op") {
    Database db(":memory:");
    db.save_graph({{1, {0,0,0}, "alpha", ""}}, {});
    db.update_node_tier(9999, "phantom");  // should not throw

    std::vector<StoredNode> out;
    std::vector<Edge>       e;
    REQUIRE(db.load_graph(out, e));
    REQUIRE(out.size() == 1);
    CHECK(out[0].id == 1);
    CHECK(out[0].tier == "confirmed");
}

TEST_CASE("db: 500-node save/load roundtrip stays correct + reasonably fast") {
    Database db(":memory:");
    std::vector<StoredNode> in;
    in.reserve(500);
    for (long long i = 0; i < 500; ++i) {
        StoredNode n{};
        n.id           = i;
        n.position     = {static_cast<float>(i), 0, 0};
        n.title        = "node-" + std::to_string(i);
        n.first_seen   = 1000.0 + static_cast<double>(i);
        n.last_touched = 2000.0 + static_cast<double>(i);
        n.tier         = (i % 3 == 0) ? "confirmed" : (i % 3 == 1 ? "suspected" : "phantom");
        in.push_back(std::move(n));
    }
    db.save_graph(in, {});

    std::vector<StoredNode> out;
    std::vector<Edge>       e;
    REQUIRE(db.load_graph(out, e));
    REQUIRE(out.size() == 500);
    CHECK(out[0].title   == "node-0");
    CHECK(out[499].title == "node-499");
    CHECK(out[42].tier   == "confirmed");
    CHECK(out[42].first_seen == doctest::Approx(1042.0));
}

TEST_CASE("db: edges with label/kind/certainty roundtrip") {
    Database db(":memory:");
    Edge e1{1, 2, "owns gun", "owns",     "confirmed"};
    Edge e2{2, 3, "saw at site B", "saw-at", "suspected"};
    Edge e3{1, 3, "", "", "hearsay"};
    db.save_graph(
        {{1, {0,0,0}, "a", ""}, {2, {0,0,0}, "b", ""}, {3, {0,0,0}, "c", ""}},
        {e1, e2, e3});

    std::vector<StoredNode> nodes;
    std::vector<Edge>       edges;
    REQUIRE(db.load_graph(nodes, edges));
    REQUIRE(edges.size() == 3);
    CHECK(edges[0].label     == "owns gun");
    CHECK(edges[0].kind      == "owns");
    CHECK(edges[0].certainty == "confirmed");
    CHECK(edges[1].label     == "saw at site B");
    CHECK(edges[1].kind      == "saw-at");
    CHECK(edges[1].certainty == "suspected");
    CHECK(edges[2].label.empty());
    CHECK(edges[2].kind.empty());
    CHECK(edges[2].certainty == "hearsay");
}

TEST_CASE("db: update_edge changes label/kind/certainty on the matching pair") {
    Database db(":memory:");
    db.save_graph(
        {{1, {0,0,0}, "a", ""}, {2, {0,0,0}, "b", ""}},
        {{1, 2, "", "", "confirmed"}});

    db.update_edge(1, 2, "shell of", "shell-of", "suspected");

    std::vector<StoredNode> nodes;
    std::vector<Edge>       edges;
    REQUIRE(db.load_graph(nodes, edges));
    REQUIRE(edges.size() == 1);
    CHECK(edges[0].label     == "shell of");
    CHECK(edges[0].kind      == "shell-of");
    CHECK(edges[0].certainty == "suspected");
}

TEST_CASE("db: update_edge on a non-existent pair is a silent no-op") {
    Database db(":memory:");
    db.save_graph(
        {{1, {0,0,0}, "a", ""}},
        {{1, 1, "self", "knows", "confirmed"}});

    db.update_edge(99, 42, "ghost", "noop", "phantom");  // no matching row

    std::vector<StoredNode> nodes;
    std::vector<Edge>       edges;
    REQUIRE(db.load_graph(nodes, edges));
    REQUIRE(edges.size() == 1);
    CHECK(edges[0].label == "self");
    CHECK(edges[0].kind  == "knows");
}

TEST_CASE("db: legacy edges table missing the new columns is migrated") {
    const std::string path = tmp_db_path("migrate_edges");
    std::remove(path.c_str());

    // Create a legacy-shape DB with only source/target/weight on edges.
    {
        sqlite3* raw = nullptr;
        REQUIRE(sqlite3_open(path.c_str(), &raw) == SQLITE_OK);
        sqlite3_exec(raw,
            "CREATE TABLE nodes ("
            "  id INTEGER PRIMARY KEY, x REAL, y REAL, z REAL,"
            "  title TEXT NOT NULL DEFAULT '', content TEXT NOT NULL DEFAULT '');"
            "CREATE TABLE edges (source INTEGER, target INTEGER, weight REAL);"
            "INSERT INTO nodes (id, x, y, z, title, content) VALUES (1, 0, 0, 0, 'a', '');"
            "INSERT INTO nodes (id, x, y, z, title, content) VALUES (2, 0, 0, 0, 'b', '');"
            "INSERT INTO edges (source, target, weight) VALUES (1, 2, 1.0);",
            nullptr, nullptr, nullptr);
        sqlite3_close(raw);
    }

    Database db(path);
    std::vector<StoredNode> nodes;
    std::vector<Edge>       edges;
    REQUIRE(db.load_graph(nodes, edges));
    REQUIRE(edges.size() == 1);
    CHECK(edges[0].label.empty());
    CHECK(edges[0].kind.empty());
    CHECK(edges[0].certainty == "confirmed");

    std::remove(path.c_str());
}

TEST_CASE("db: roundtrip preserves edges with negative-looking large weights (legacy stored)") {
    // Verifies that the weight column we silently default to 1.0 on save
    // doesn't pollute caller's edge state. Load returns Edges without a
    // weight field (Edge struct never carried it) — confirm round-trip is
    // stable across save/load even when edges originally had varied counts.
    Database db(":memory:");
    std::vector<Edge> in_edges;
    for (long long i = 0; i < 20; ++i) {
        in_edges.push_back({static_cast<std::size_t>(i),
                            static_cast<std::size_t>(i + 1), "", "", "confirmed"});
    }
    std::vector<StoredNode> in_nodes;
    for (long long i = 0; i < 22; ++i) {
        in_nodes.push_back({i, {0,0,0}, "n" + std::to_string(i), ""});
    }
    db.save_graph(in_nodes, in_edges);

    std::vector<StoredNode> out_nodes;
    std::vector<Edge>       out_edges;
    REQUIRE(db.load_graph(out_nodes, out_edges));
    CHECK(out_edges.size() == in_edges.size());
    for (std::size_t i = 0; i < in_edges.size(); ++i) {
        CHECK(out_edges[i].source == in_edges[i].source);
        CHECK(out_edges[i].target == in_edges[i].target);
    }
}

TEST_CASE("db: opening the same file twice doesn't re-migrate or corrupt data") {
    // Migration is gated on column existence; opening an already-migrated
    // file should be idempotent.
    const std::string path = tmp_db_path("idempotent");
    std::remove(path.c_str());

    {
        Database db(path);
        db.save_graph({{1, {0,0,0}, "stable", "body"}}, {{1, 1, "self-link", "knows", "confirmed"}});
    }
    {
        Database db(path);  // second open, should not re-migrate
        std::vector<StoredNode> nodes;
        std::vector<Edge>       edges;
        REQUIRE(db.load_graph(nodes, edges));
        REQUIRE(nodes.size() == 1);
        CHECK(nodes[0].title == "stable");
        REQUIRE(edges.size() == 1);
        CHECK(edges[0].label == "self-link");
    }
    {
        Database db(path);  // third open
        std::vector<StoredNode> nodes;
        std::vector<Edge>       edges;
        REQUIRE(db.load_graph(nodes, edges));
        CHECK(nodes[0].title == "stable");
    }

    std::remove(path.c_str());
}

TEST_CASE("db: edge label of 4 KB roundtrips byte-exact") {
    Database db(":memory:");
    std::string huge_label(4096, 'L');
    db.save_graph(
        {{1, {0,0,0}, "a", ""}, {2, {0,0,0}, "b", ""}},
        {{1, 2, huge_label, "knows", "confirmed"}});

    std::vector<StoredNode> nodes;
    std::vector<Edge>       edges;
    REQUIRE(db.load_graph(nodes, edges));
    REQUIRE(edges.size() == 1);
    CHECK(edges[0].label.size() == 4096);
    CHECK(edges[0].label == huge_label);
}

TEST_CASE("db: node tags roundtrip through save_graph and load_graph") {
    Database db(":memory:");
    StoredNode n1{};
    n1.id = 1; n1.position = {0,0,0}; n1.title = "alpha";
    n1.tags = {"subject", "asset"};
    StoredNode n2{};
    n2.id = 2; n2.position = {0,0,0}; n2.title = "beta";
    n2.tags = {};  // no tags
    StoredNode n3{};
    n3.id = 3; n3.position = {0,0,0}; n3.title = "gamma";
    n3.tags = {"hostile"};

    db.save_graph({n1, n2, n3}, {});

    std::vector<StoredNode> out;
    std::vector<Edge>       edges;
    REQUIRE(db.load_graph(out, edges));
    REQUIRE(out.size() == 3);
    CHECK(out[0].tags.size() == 2);
    CHECK((out[0].tags[0] == "subject" || out[0].tags[0] == "asset"));
    CHECK(out[1].tags.empty());
    REQUIRE(out[2].tags.size() == 1);
    CHECK(out[2].tags[0] == "hostile");
}

TEST_CASE("db: duplicate tags within a node's list collapse to one stored row") {
    Database db(":memory:");
    StoredNode n{};
    n.id = 1; n.position = {0,0,0}; n.title = "a";
    n.tags = {"asset", "asset", "asset"};  // 3 copies
    db.save_graph({n}, {});

    std::vector<StoredNode> out;
    std::vector<Edge>       edges;
    REQUIRE(db.load_graph(out, edges));
    REQUIRE(out.size() == 1);
    // The PK (node_id, tag) collapses dup inserts; loaded set has just one.
    CHECK(out[0].tags.size() == 1);
    CHECK(out[0].tags[0] == "asset");
}

TEST_CASE("db: update_node_tags atomically replaces the tag set") {
    Database db(":memory:");
    StoredNode n{};
    n.id = 1; n.position = {0,0,0}; n.title = "a";
    n.tags = {"old1", "old2"};
    db.save_graph({n}, {});

    db.update_node_tags(1, {"new1", "new2", "new3"});

    std::vector<StoredNode> out;
    std::vector<Edge>       edges;
    REQUIRE(db.load_graph(out, edges));
    REQUIRE(out.size() == 1);
    REQUIRE(out[0].tags.size() == 3);
    std::sort(out[0].tags.begin(), out[0].tags.end());
    CHECK(out[0].tags[0] == "new1");
    CHECK(out[0].tags[1] == "new2");
    CHECK(out[0].tags[2] == "new3");
}

TEST_CASE("db: update_node_tags with empty vector clears all tags") {
    Database db(":memory:");
    StoredNode n{};
    n.id = 1; n.position = {0,0,0}; n.title = "a";
    n.tags = {"keep", "kill"};
    db.save_graph({n}, {});

    db.update_node_tags(1, {});

    std::vector<StoredNode> out;
    std::vector<Edge>       edges;
    REQUIRE(db.load_graph(out, edges));
    CHECK(out[0].tags.empty());
}

TEST_CASE("db: tags survive a legacy DB without node_tags table (migration creates it)") {
    const std::string path = tmp_db_path("tags_migrate");
    std::remove(path.c_str());

    // Legacy schema: no node_tags.
    {
        sqlite3* raw = nullptr;
        REQUIRE(sqlite3_open(path.c_str(), &raw) == SQLITE_OK);
        sqlite3_exec(raw,
            "CREATE TABLE nodes ("
            "  id INTEGER PRIMARY KEY, x REAL, y REAL, z REAL,"
            "  title TEXT NOT NULL DEFAULT '', content TEXT NOT NULL DEFAULT '');"
            "INSERT INTO nodes (id, x, y, z, title, content) VALUES (1, 0, 0, 0, 'legacy', '');",
            nullptr, nullptr, nullptr);
        sqlite3_close(raw);
    }

    Database db(path);
    db.update_node_tags(1, {"freshly-added"});

    std::vector<StoredNode> out;
    std::vector<Edge>       edges;
    REQUIRE(db.load_graph(out, edges));
    REQUIRE(out.size() == 1);
    REQUIRE(out[0].tags.size() == 1);
    CHECK(out[0].tags[0] == "freshly-added");

    std::remove(path.c_str());
}

TEST_CASE("db: meta_double returns fallback when key is missing") {
    Database db(":memory:");
    CHECK(db.meta_double("never-set", -1.0) == doctest::Approx(-1.0));
    CHECK(db.meta_double("never-set",  0.0) == doctest::Approx( 0.0));
}

TEST_CASE("db: meta_double roundtrips through set_meta_double") {
    Database db(":memory:");
    db.set_meta_double("last_open_ts", 1735689600.5);
    CHECK(db.meta_double("last_open_ts", 0.0) == doctest::Approx(1735689600.5));
}

TEST_CASE("db: set_meta_double upserts (second write overwrites)") {
    Database db(":memory:");
    db.set_meta_double("k", 1.0);
    db.set_meta_double("k", 2.0);
    db.set_meta_double("k", 3.0);
    CHECK(db.meta_double("k", -1.0) == doctest::Approx(3.0));
}

TEST_CASE("db: meta survives close + reopen against a real file") {
    const std::string path = tmp_db_path("meta");
    std::remove(path.c_str());

    {
        Database db(path);
        db.set_meta_double("last_open_ts", 9876.5);
    }
    {
        Database db(path);  // fresh connection
        CHECK(db.meta_double("last_open_ts", 0.0) == doctest::Approx(9876.5));
    }
    std::remove(path.c_str());
}

TEST_CASE("db: update_node_tier with empty string is stored verbatim") {
    Database db(":memory:");
    db.save_graph({{1, {0,0,0}, "alpha", ""}}, {});

    db.update_node_tier(1, "");

    std::vector<StoredNode> out;
    std::vector<Edge>       e;
    REQUIRE(db.load_graph(out, e));
    CHECK(out[0].tier.empty());
}

TEST_CASE("db: 4KB title roundtrips byte-exact") {
    Database db(":memory:");
    std::string long_title(4096, 'A');
    db.save_graph({{1, {0,0,0}, long_title, ""}}, {});

    std::vector<StoredNode> out;
    std::vector<Edge>       e;
    REQUIRE(db.load_graph(out, e));
    CHECK(out[0].title.size() == 4096);
    CHECK(out[0].title == long_title);
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

TEST_CASE("db: log_event inserts rows with kind, node_id, payload and a unix-seconds ts") {
    Database db(":memory:");
    sqlite3* raw = nullptr;
    // Open a separate connection to verify what log_event wrote. ":memory:"
    // databases are connection-private, so use a temp file path instead.
    const std::string path = tmp_db_path("events");
    std::remove(path.c_str());
    {
        Database fdb(path);
        fdb.log_event("phantom_spawn", 42, "{\"label\":\"alice\"}");
        fdb.log_event("phantom_pin",   42, "{\"new_id\":42}");
        fdb.log_event("bones_throw",  -1, "{\"chosen\":[1,2,3]}");
    }

    REQUIRE(sqlite3_open(path.c_str(), &raw) == SQLITE_OK);
    sqlite3_stmt* stmt = nullptr;
    REQUIRE(sqlite3_prepare_v2(raw,
        "SELECT ts, kind, node_id, payload FROM events ORDER BY rowid;",
        -1, &stmt, nullptr) == SQLITE_OK);

    // row 0: phantom_spawn for node 42
    REQUIRE(sqlite3_step(stmt) == SQLITE_ROW);
    const double ts0 = sqlite3_column_double(stmt, 0);
    CHECK(std::string(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1)))
          == "phantom_spawn");
    CHECK(sqlite3_column_int64(stmt, 2) == 42);
    CHECK(std::string(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3)))
          == "{\"label\":\"alice\"}");
    // ts is unix seconds; should land somewhere between 2020-01-01 and the
    // year 2100. Tight enough to catch a unit mistake (julianday vs unix),
    // loose enough to survive long after this is written.
    CHECK(ts0 > 1577836800.0);   // 2020-01-01 UTC
    CHECK(ts0 < 4102444800.0);   // 2100-01-01 UTC

    // row 1: phantom_pin
    REQUIRE(sqlite3_step(stmt) == SQLITE_ROW);
    CHECK(std::string(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1)))
          == "phantom_pin");

    // row 2: bones_throw with NULL node_id (we passed -1)
    REQUIRE(sqlite3_step(stmt) == SQLITE_ROW);
    CHECK(std::string(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1)))
          == "bones_throw");
    CHECK(sqlite3_column_type(stmt, 2) == SQLITE_NULL);
    CHECK(std::string(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3)))
          == "{\"chosen\":[1,2,3]}");

    REQUIRE(sqlite3_step(stmt) == SQLITE_DONE);
    sqlite3_finalize(stmt);
    sqlite3_close(raw);
    std::remove(path.c_str());
}

TEST_CASE("db: deleted flag roundtrips through save/load") {
    Database db(":memory:");
    std::vector<StoredNode> in_nodes = {
        {0, {0, 0, 0}, "alpha", "",  0, 0, "confirmed", {}, /*deleted=*/false},
        {1, {1, 0, 0}, "beta",  "",  0, 0, "confirmed", {}, /*deleted=*/true},
        {2, {2, 0, 0}, "gamma", "",  0, 0, "confirmed", {}, /*deleted=*/false},
    };
    db.save_graph(in_nodes, {});

    std::vector<StoredNode> out_nodes;
    std::vector<Edge>       out_edges;
    REQUIRE(db.load_graph(out_nodes, out_edges));
    REQUIRE(out_nodes.size() == 3);
    CHECK_FALSE(out_nodes[0].deleted);
    CHECK      (out_nodes[1].deleted);
    CHECK_FALSE(out_nodes[2].deleted);
}

TEST_CASE("db: legacy DB without the deleted column migrates and loads with deleted=false") {
    // Simulate a pre-deleted-column DB by creating the nodes table without
    // that column, inserting a row, then opening it through Database which
    // should run the ALTER TABLE migration and leave the row with
    // deleted=false (the column's DEFAULT).
    const std::string path = tmp_db_path("legacy");
    std::remove(path.c_str());
    {
        sqlite3* raw = nullptr;
        REQUIRE(sqlite3_open(path.c_str(), &raw) == SQLITE_OK);
        const char* legacy_schema =
            "CREATE TABLE nodes ("
            "  id INTEGER PRIMARY KEY,"
            "  x REAL NOT NULL, y REAL NOT NULL, z REAL NOT NULL,"
            "  title TEXT NOT NULL DEFAULT '',"
            "  content TEXT NOT NULL DEFAULT ''"
            ");";
        char* err = nullptr;
        REQUIRE(sqlite3_exec(raw, legacy_schema, nullptr, nullptr, &err) == SQLITE_OK);
        REQUIRE(sqlite3_exec(raw,
            "INSERT INTO nodes (id, x, y, z, title) VALUES (42, 0, 0, 0, 'old');",
            nullptr, nullptr, &err) == SQLITE_OK);
        sqlite3_close(raw);
    }

    Database db(path);  // migration runs here
    std::vector<StoredNode> out_nodes;
    std::vector<Edge>       out_edges;
    REQUIRE(db.load_graph(out_nodes, out_edges));
    REQUIRE(out_nodes.size() == 1);
    CHECK(out_nodes[0].id == 42);
    CHECK_FALSE(out_nodes[0].deleted);

    std::remove(path.c_str());
}

TEST_CASE("db: log_event timestamps are monotonically non-decreasing") {
    Database db(":memory:");
    for (int i = 0; i < 50; ++i) {
        db.log_event("tick", i, "");
    }
    sqlite3* raw = nullptr;
    // re-prep on a temp file so we can read events back. Skip if we're
    // running against :memory: in this test variant.
    const std::string path = tmp_db_path("events2");
    std::remove(path.c_str());
    {
        Database fdb(path);
        for (int i = 0; i < 50; ++i) fdb.log_event("tick", i, "");
    }
    REQUIRE(sqlite3_open(path.c_str(), &raw) == SQLITE_OK);
    sqlite3_stmt* stmt = nullptr;
    REQUIRE(sqlite3_prepare_v2(raw,
        "SELECT ts FROM events ORDER BY rowid;",
        -1, &stmt, nullptr) == SQLITE_OK);

    double prev = -1.0;
    int rows = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const double ts = sqlite3_column_double(stmt, 0);
        CHECK(ts >= prev);
        prev = ts;
        ++rows;
    }
    CHECK(rows == 50);
    sqlite3_finalize(stmt);
    sqlite3_close(raw);
    std::remove(path.c_str());
}
