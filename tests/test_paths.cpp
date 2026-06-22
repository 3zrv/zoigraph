#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "app/paths.h"

#include <filesystem>
#include <fstream>

using zg::app::resolve_resource;
namespace fs = std::filesystem;

TEST_CASE("resolve_resource: cwd wins, then exe, then cwd fallback") {
    const fs::path base = fs::temp_directory_path() / "zoigraph_paths_test";
    std::error_code ec;
    fs::remove_all(base, ec);
    const fs::path cwd = base / "cwd";
    const fs::path exe = base / "exe";
    fs::create_directories(cwd);
    fs::create_directories(exe);

    // Neither root has it -> the cwd-relative fallback (which need not exist).
    CHECK(resolve_resource("scripts", cwd, exe) == cwd / "scripts");

    // Only the exe dir has it -> the exe path (the relocated-tarball case).
    fs::create_directory(exe / "scripts");
    CHECK(resolve_resource("scripts", cwd, exe) == exe / "scripts");

    // The cwd has it too -> cwd wins (a dev run from the repo root).
    fs::create_directory(cwd / "scripts");
    CHECK(resolve_resource("scripts", cwd, exe) == cwd / "scripts");

    // A name with subdirectories resolves against the same roots.
    fs::create_directories(exe / "scripts");  // already there
    std::ofstream(exe / "scripts" / "llm.py").put('x');
    CHECK(resolve_resource("scripts/llm.py", cwd, exe) == exe / "scripts" / "llm.py");

    // Empty exe_dir skips step 2 -> cwd fallback.
    CHECK(resolve_resource("projects", cwd, fs::path{}) == cwd / "projects");

    fs::remove_all(base, ec);
}
