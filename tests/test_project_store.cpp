#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "persistence/db.h"
#include "persistence/project_store.h"

#include <filesystem>
#include <fstream>
#include <string>

// Cross-platform PID for unique temp-dir naming. _getpid on Windows and
// getpid on POSIX share the same signature.
#if defined(_WIN32)
    #include <process.h>
    #define ZG_GETPID() _getpid()
#else
    #include <unistd.h>
    #define ZG_GETPID() ::getpid()
#endif

namespace fs = std::filesystem;
using namespace zg::persistence;

namespace {

fs::path tmp_dir(const std::string& tag) {
    fs::path p = fs::temp_directory_path() /
                 ("zg_ps_" + tag + "_" + std::to_string(ZG_GETPID()));
    fs::remove_all(p);
    return p;
}

void touch_db(const fs::path& p) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p);
    f << "fake";
}

}  // namespace

TEST_CASE("project_store: name validation accepts alnum + dash + underscore") {
    CHECK(is_valid_project_name("default"));
    CHECK(is_valid_project_name("red-string"));
    CHECK(is_valid_project_name("op_42"));
    CHECK(is_valid_project_name("A"));
    CHECK(is_valid_project_name(std::string(64, 'a')));
}

TEST_CASE("project_store: name validation rejects empties, overlong, and dangerous") {
    CHECK_FALSE(is_valid_project_name(""));
    CHECK_FALSE(is_valid_project_name(std::string(65, 'a')));
    CHECK_FALSE(is_valid_project_name("with space"));
    CHECK_FALSE(is_valid_project_name("with/slash"));
    CHECK_FALSE(is_valid_project_name(".."));
    CHECK_FALSE(is_valid_project_name(".hidden"));
    CHECK_FALSE(is_valid_project_name("with.dot"));
    CHECK_FALSE(is_valid_project_name("café"));  // unicode rejected
}

TEST_CASE("project_store: list_projects on a missing dir returns empty") {
    const auto dir = tmp_dir("missing");
    CHECK(list_projects(dir).empty());
}

TEST_CASE("project_store: list_projects returns sorted *.db basenames; skips hidden") {
    const auto dir = tmp_dir("list");
    touch_db(dir / "beta.db");
    touch_db(dir / "alpha.db");
    touch_db(dir / "gamma.db");
    touch_db(dir / "notes.txt");   // skipped: not .db
    touch_db(dir / ".secret.db");  // skipped: hidden
    std::ofstream(dir / ".last") << "alpha";  // sidecar, not a project

    const auto got = list_projects(dir);
    REQUIRE(got.size() == 3);
    CHECK(got[0] == "alpha");
    CHECK(got[1] == "beta");
    CHECK(got[2] == "gamma");

    fs::remove_all(dir);
}

TEST_CASE("project_store: project_path concatenates dir + name + .db") {
    const fs::path dir = "/tmp/xyz";
    // generic_string() always uses forward slashes; .string() on Windows
    // returns the native '\' separator so the equality would otherwise
    // fail on the windows-latest CI runner.
    CHECK(project_path(dir, "alpha").generic_string() == "/tmp/xyz/alpha.db");
}

TEST_CASE("project_store: write_last + read_last round-trips") {
    const auto dir = tmp_dir("last");
    fs::create_directories(dir);
    write_last_project(dir, "my-project");
    CHECK(read_last_project(dir, "fallback") == "my-project");
    fs::remove_all(dir);
}

TEST_CASE("project_store: read_last returns fallback when file is missing or invalid") {
    const auto dir = tmp_dir("fallback");
    fs::create_directories(dir);

    // Missing file -> fallback.
    CHECK(read_last_project(dir, "default") == "default");

    // File with whitespace-only -> fallback.
    std::ofstream(dir / ".last") << "   \n";
    CHECK(read_last_project(dir, "default") == "default");

    // File with invalid name -> fallback.
    std::ofstream(dir / ".last", std::ios::trunc) << "../escape\n";
    CHECK(read_last_project(dir, "default") == "default");

    fs::remove_all(dir);
}

TEST_CASE("project_store: write_last rejects invalid names silently") {
    const auto dir = tmp_dir("badwrite");
    fs::create_directories(dir);
    write_last_project(dir, "../escape");
    CHECK_FALSE(fs::exists(dir / ".last"));
    fs::remove_all(dir);
}

TEST_CASE("project_store: delete_project removes the .db file and reports it") {
    const auto dir = tmp_dir("del");
    touch_db(dir / "doomed.db");
    REQUIRE(fs::exists(dir / "doomed.db"));

    CHECK(delete_project(dir, "doomed"));
    CHECK_FALSE(fs::exists(dir / "doomed.db"));

    // Second delete returns false (nothing to remove).
    CHECK_FALSE(delete_project(dir, "doomed"));
    fs::remove_all(dir);
}

TEST_CASE("project_store: migrate_legacy_db moves zoigraph.db -> projects/default.db") {
    const auto base = tmp_dir("migrate");
    fs::create_directories(base);
    const fs::path legacy = base / "zoigraph.db";
    const fs::path projects = base / "projects";
    touch_db(legacy);

    migrate_legacy_db(legacy, projects, "default");

    CHECK_FALSE(fs::exists(legacy));
    CHECK(fs::exists(projects / "default.db"));

    fs::remove_all(base);
}

TEST_CASE("project_store: migrate is a no-op when projects/default.db already exists") {
    const auto base = tmp_dir("migrate_noop");
    fs::create_directories(base);
    const fs::path legacy = base / "zoigraph.db";
    const fs::path projects = base / "projects";
    touch_db(legacy);
    touch_db(projects / "default.db");

    migrate_legacy_db(legacy, projects, "default");

    CHECK(fs::exists(legacy));               // not moved
    CHECK(fs::exists(projects / "default.db"));
    fs::remove_all(base);
}

TEST_CASE("project_store: migrate creates projects/ even when nothing to move") {
    const auto base = tmp_dir("migrate_empty");
    const fs::path projects = base / "projects";
    migrate_legacy_db(base / "zoigraph.db", projects, "default");
    CHECK(fs::exists(projects));
    fs::remove_all(base);
}

// ---- Integration: project_store + Database --------------------------------
// These exercise the actual "create new project" flow from main.cpp: open a
// Database at the project_path, persist data, list_projects reports it,
// reopening the same path preserves the data. The unit tests above stub the
// DB with `touch_db`; here we run the real SQLite path.

TEST_CASE("integration: creating a Database at project_path makes it discoverable") {
    const auto dir = tmp_dir("integ_create");
    fs::create_directories(dir);
    REQUIRE(list_projects(dir).empty());

    {
        zg::persistence::Database db(project_path(dir, "alpha").string());
        db.save_graph(
            {{1, {1, 2, 3}, "first-note", "body"}},
            {});
    }

    const auto names = list_projects(dir);
    REQUIRE(names.size() == 1);
    CHECK(names[0] == "alpha");

    // Reopen and verify the data round-trips through the on-disk file.
    {
        zg::persistence::Database db(project_path(dir, "alpha").string());
        std::vector<zg::persistence::StoredNode> nodes;
        std::vector<zg::graph::Edge>             edges;
        REQUIRE(db.load_graph(nodes, edges));
        REQUIRE(nodes.size() == 1);
        CHECK(nodes[0].title == "first-note");
    }

    fs::remove_all(dir);
}

TEST_CASE("integration: two projects in the same directory are fully isolated") {
    const auto dir = tmp_dir("integ_isolated");
    fs::create_directories(dir);

    {
        zg::persistence::Database alpha(project_path(dir, "alpha").string());
        alpha.save_graph({{1, {0,0,0}, "from-alpha", ""}}, {});
    }
    {
        zg::persistence::Database beta(project_path(dir, "beta").string());
        beta.save_graph({{1, {0,0,0}, "from-beta", ""}}, {});
    }

    const auto names = list_projects(dir);
    REQUIRE(names.size() == 2);
    CHECK(names[0] == "alpha");
    CHECK(names[1] == "beta");

    {
        zg::persistence::Database alpha(project_path(dir, "alpha").string());
        std::vector<zg::persistence::StoredNode> nodes;
        std::vector<zg::graph::Edge>             edges;
        REQUIRE(alpha.load_graph(nodes, edges));
        CHECK(nodes[0].title == "from-alpha");
    }
    {
        zg::persistence::Database beta(project_path(dir, "beta").string());
        std::vector<zg::persistence::StoredNode> nodes;
        std::vector<zg::graph::Edge>             edges;
        REQUIRE(beta.load_graph(nodes, edges));
        CHECK(nodes[0].title == "from-beta");
    }

    fs::remove_all(dir);
}

TEST_CASE("integration: delete_project removes only the named project's files") {
    const auto dir = tmp_dir("integ_delete");
    fs::create_directories(dir);

    {
        zg::persistence::Database a(project_path(dir, "alpha").string());
        a.save_graph({{1, {0,0,0}, "alpha", ""}}, {});
    }
    {
        zg::persistence::Database b(project_path(dir, "beta").string());
        b.save_graph({{1, {0,0,0}, "beta", ""}}, {});
    }
    REQUIRE(list_projects(dir).size() == 2);

    CHECK(delete_project(dir, "alpha"));
    const auto names = list_projects(dir);
    REQUIRE(names.size() == 1);
    CHECK(names[0] == "beta");
    CHECK_FALSE(fs::exists(project_path(dir, "alpha")));

    fs::remove_all(dir);
}

TEST_CASE("project_store: create + delete + create same name leaves no residue") {
    const auto dir = tmp_dir("ping_pong");
    fs::create_directories(dir);

    for (int i = 0; i < 5; ++i) {
        touch_db(dir / "scratch.db");
        REQUIRE(delete_project(dir, "scratch"));
        CHECK_FALSE(fs::exists(dir / "scratch.db"));
    }
    CHECK(list_projects(dir).empty());

    fs::remove_all(dir);
}

TEST_CASE("project_store: write_last + list_projects survive being called many times") {
    const auto dir = tmp_dir("loop");
    fs::create_directories(dir);
    for (int i = 0; i < 50; ++i) {
        write_last_project(dir, "spam");
    }
    CHECK(read_last_project(dir, "fallback") == "spam");
    fs::remove_all(dir);
}

TEST_CASE("integration: project_path('proj/a/../b') is still rejected up front") {
    // is_valid_project_name guards the entire pipeline — once a name fails
    // validation, none of the downstream functions should touch the disk.
    CHECK_FALSE(is_valid_project_name("a/../b"));
    CHECK_FALSE(is_valid_project_name("../escape"));
    CHECK_FALSE(is_valid_project_name(".last"));
}
