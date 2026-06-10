#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "app/settings.h"

#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;
using zg::app::load_settings;
using zg::app::save_settings;
using zg::app::Settings;

namespace {

fs::path scratch_file(const char* name) {
    const fs::path dir = fs::temp_directory_path() / "zg_settings_tests";
    fs::create_directories(dir);
    const fs::path p = dir / name;
    fs::remove(p);
    return p;
}

void write_file(const fs::path& p, const std::string& content) {
    std::ofstream out(p);
    out << content;
}

}  // namespace

TEST_CASE("settings: missing file yields defaults") {
    const Settings s = load_settings(scratch_file("missing.json"));
    CHECK(s.show_grid);
    CHECK(s.post_process);
    CHECK(s.dim_filtered);
    CHECK(s.telemetry_port == 7777);
}

TEST_CASE("settings: save/load roundtrip preserves every field") {
    const auto p = scratch_file("roundtrip.json");
    Settings s;
    s.show_grid      = false;
    s.post_process   = false;
    s.dim_filtered   = false;
    s.telemetry_port = 9999;
    REQUIRE(save_settings(p, s));

    const Settings r = load_settings(p);
    CHECK_FALSE(r.show_grid);
    CHECK_FALSE(r.post_process);
    CHECK_FALSE(r.dim_filtered);
    CHECK(r.telemetry_port == 9999);
}

TEST_CASE("settings: corrupt JSON yields defaults") {
    const auto p = scratch_file("corrupt.json");
    write_file(p, "{not json at all");
    const Settings s = load_settings(p);
    CHECK(s.telemetry_port == 7777);
    CHECK(s.show_grid);
}

TEST_CASE("settings: non-object JSON yields defaults") {
    const auto p = scratch_file("array.json");
    write_file(p, "[1, 2, 3]");
    CHECK(load_settings(p).telemetry_port == 7777);
}

TEST_CASE("settings: partial file fills the rest with defaults") {
    const auto p = scratch_file("partial.json");
    write_file(p, R"({"show_grid": false})");
    const Settings s = load_settings(p);
    CHECK_FALSE(s.show_grid);
    CHECK(s.post_process);       // untouched default
    CHECK(s.telemetry_port == 7777);
}

TEST_CASE("settings: wrong-typed keys fall back per key") {
    const auto p = scratch_file("badtypes.json");
    write_file(p, R"({"show_grid": "yes", "telemetry_port": "7777", "dim_filtered": false})");
    const Settings s = load_settings(p);
    CHECK(s.show_grid);               // string -> default
    CHECK(s.telemetry_port == 7777);  // string -> default
    CHECK_FALSE(s.dim_filtered);      // valid key still applies
}

TEST_CASE("settings: out-of-range port falls back to default") {
    const auto p = scratch_file("badport.json");
    write_file(p, R"({"telemetry_port": 0})");
    CHECK(load_settings(p).telemetry_port == 7777);
    write_file(p, R"({"telemetry_port": 70000})");
    CHECK(load_settings(p).telemetry_port == 7777);
    write_file(p, R"({"telemetry_port": -5})");
    CHECK(load_settings(p).telemetry_port == 7777);
}
