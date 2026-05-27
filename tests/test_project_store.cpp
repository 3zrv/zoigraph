#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "persistence/project_store.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <sys/types.h>
#include <unistd.h>

namespace fs = std::filesystem;
using namespace zg::persistence;

namespace {

fs::path tmp_dir(const std::string& tag) {
    fs::path p = fs::temp_directory_path() /
                 ("zg_ps_" + tag + "_" + std::to_string(::getpid()));
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
    CHECK(project_path(dir, "alpha").string() == "/tmp/xyz/alpha.db");
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
