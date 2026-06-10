#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "persistence/secure_wipe.h"

#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

namespace {

// Fresh scratch dir per test case so cases can't contaminate each other.
fs::path scratch_dir(const char* name) {
    const fs::path dir = fs::temp_directory_path() / "zg_secure_wipe_tests" / name;
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir;
}

void write_file(const fs::path& p, const std::string& content) {
    std::ofstream out(p, std::ios::binary);
    out << content;
}

std::string read_file(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in),
                       std::istreambuf_iterator<char>());
}

}  // namespace

TEST_CASE("secure_overwrite_file: missing file returns false") {
    const auto dir = scratch_dir("overwrite_missing");
    CHECK_FALSE(zg::persistence::secure_overwrite_file(dir / "nope.db"));
}

TEST_CASE("secure_overwrite_file: zeroes every byte, preserves size, keeps the file") {
    const auto dir = scratch_dir("overwrite_basic");
    const auto p   = dir / "secrets.db";
    const std::string original = "operator threat model: the works";
    write_file(p, original);

    CHECK(zg::persistence::secure_overwrite_file(p));

    REQUIRE(fs::exists(p));
    const std::string after = read_file(p);
    REQUIRE(after.size() == original.size());
    for (char c : after) CHECK(c == '\0');
}

TEST_CASE("secure_overwrite_file: empty file is a no-op success") {
    const auto dir = scratch_dir("overwrite_empty");
    const auto p   = dir / "empty.db";
    write_file(p, "");
    CHECK(zg::persistence::secure_overwrite_file(p));
    CHECK(fs::exists(p));
}

TEST_CASE("secure_wipe_file: overwrites then removes the file") {
    const auto dir = scratch_dir("wipe_basic");
    const auto p   = dir / "secrets.db";
    write_file(p, "sensitive");

    CHECK(zg::persistence::secure_wipe_file(p));
    CHECK_FALSE(fs::exists(p));
}

TEST_CASE("secure_wipe_file: missing file returns false") {
    const auto dir = scratch_dir("wipe_missing");
    CHECK_FALSE(zg::persistence::secure_wipe_file(dir / "nope.db"));
}

TEST_CASE("panic_wipe: missing directory wipes nothing") {
    const auto dir = scratch_dir("panic_missing");
    CHECK(zg::persistence::panic_wipe(dir / "projects") == 0);
}

TEST_CASE("panic_wipe: wipes every file recursively and removes the tree") {
    const auto dir  = scratch_dir("panic_tree");
    const auto root = dir / "projects";
    fs::create_directories(root / "nested");
    write_file(root / "default.db", "graph A");
    write_file(root / "default.db-wal", "wal frames");
    write_file(root / "ops.db", "graph B");
    write_file(root / ".last", "ops");
    write_file(root / "nested" / "stray.txt", "leftover");

    CHECK(zg::persistence::panic_wipe(root) == 5);
    CHECK_FALSE(fs::exists(root));
}

TEST_CASE("panic_wipe: leaves siblings of the target directory alone") {
    const auto dir  = scratch_dir("panic_sibling");
    const auto root = dir / "projects";
    fs::create_directories(root);
    write_file(root / "default.db", "graph");
    write_file(dir / "unrelated.txt", "keep me");

    CHECK(zg::persistence::panic_wipe(root) == 1);
    CHECK_FALSE(fs::exists(root));
    CHECK(fs::exists(dir / "unrelated.txt"));
    CHECK(read_file(dir / "unrelated.txt") == "keep me");
}
