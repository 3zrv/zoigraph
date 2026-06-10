#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "app/phantom_lifecycle.h"

using zg::app::foreign_phantom_indices;
using zg::app::phantom_lifecycle_diff;
using zg::telemetry::Phantom;

namespace {
Phantom mk(long long id, double spawn_ts) {
    Phantom p;
    p.id = id;
    p.spawn_time = spawn_ts;
    p.position = {0, 0, 0};
    return p;
}

Phantom mk_proj(long long id, const char* project) {
    Phantom p = mk(id, 0.0);
    p.project = project;
    return p;
}
}

TEST_CASE("foreign_phantom_indices: untagged phantoms always pass") {
    std::vector<Phantom> cur = {mk(1, 0.0), mk(2, 0.0)};
    CHECK(foreign_phantom_indices(cur, "alpha").empty());
}

TEST_CASE("foreign_phantom_indices: matching project passes") {
    std::vector<Phantom> cur = {mk_proj(1, "alpha")};
    CHECK(foreign_phantom_indices(cur, "alpha").empty());
}

TEST_CASE("foreign_phantom_indices: mismatched project is foreign") {
    std::vector<Phantom> cur = {mk_proj(1, "beta")};
    const auto f = foreign_phantom_indices(cur, "alpha");
    REQUIRE(f.size() == 1);
    CHECK(f[0] == 0);
}

TEST_CASE("foreign_phantom_indices: empty active project drops nothing") {
    // No project context to compare against -- be conservative, keep all.
    std::vector<Phantom> cur = {mk_proj(1, "beta"), mk(2, 0.0)};
    CHECK(foreign_phantom_indices(cur, "").empty());
}

TEST_CASE("foreign_phantom_indices: mixed list yields ascending indices "
          "of only the foreign entries") {
    std::vector<Phantom> cur = {
        mk_proj(1, "beta"),   // foreign
        mk(2, 0.0),           // untagged -> passes
        mk_proj(3, "alpha"),  // matches -> passes
        mk_proj(4, "gamma"),  // foreign
    };
    const auto f = foreign_phantom_indices(cur, "alpha");
    REQUIRE(f.size() == 2);
    CHECK(f[0] == 0);
    CHECK(f[1] == 3);
}

TEST_CASE("phantom_lifecycle_diff: empty in, empty out") {
    std::unordered_map<long long, double> seen;
    std::vector<Phantom> current;
    const auto d = phantom_lifecycle_diff(seen, current, /*now=*/100.0);
    CHECK(d.new_indices.empty());
    CHECK(d.departed.empty());
    CHECK(seen.empty());
}

TEST_CASE("phantom_lifecycle_diff: all-new phantoms appear in new_indices "
          "and get inserted into seen") {
    std::unordered_map<long long, double> seen;
    std::vector<Phantom> current = {mk(1, 10.0), mk(2, 11.0), mk(3, 12.0)};
    const auto d = phantom_lifecycle_diff(seen, current, /*now=*/100.0);
    REQUIRE(d.new_indices.size() == 3);
    CHECK(d.new_indices[0] == 0u);
    CHECK(d.new_indices[1] == 1u);
    CHECK(d.new_indices[2] == 2u);
    CHECK(d.departed.empty());
    CHECK(seen.size() == 3);
    CHECK(seen.at(1) == 10.0);
    CHECK(seen.at(2) == 11.0);
    CHECK(seen.at(3) == 12.0);
}

TEST_CASE("phantom_lifecycle_diff: empty current with non-empty seen "
          "produces departures with correct lifetime + clears seen") {
    std::unordered_map<long long, double> seen = {{1, 10.0}, {2, 30.0}};
    std::vector<Phantom> current;
    const auto d = phantom_lifecycle_diff(seen, current, /*now=*/100.0);
    CHECK(d.new_indices.empty());
    REQUIRE(d.departed.size() == 2);
    // Order is map-iteration order (unspecified); verify by sorted lookup.
    bool saw_1 = false;
    bool saw_2 = false;
    for (const auto& [id, life] : d.departed) {
        if (id == 1) { CHECK(life == doctest::Approx(90.0)); saw_1 = true; }
        if (id == 2) { CHECK(life == doctest::Approx(70.0)); saw_2 = true; }
    }
    CHECK(saw_1);
    CHECK(saw_2);
    CHECK(seen.empty());
}

TEST_CASE("phantom_lifecycle_diff: idempotent -- same seen, same current = empty delta") {
    std::unordered_map<long long, double> seen = {{1, 10.0}, {2, 11.0}};
    std::vector<Phantom> current = {mk(1, 10.0), mk(2, 11.0)};
    const auto d = phantom_lifecycle_diff(seen, current, 100.0);
    CHECK(d.new_indices.empty());
    CHECK(d.departed.empty());
    CHECK(seen.size() == 2);
}

TEST_CASE("phantom_lifecycle_diff: mixed -- some new, some persistent, some departed") {
    std::unordered_map<long long, double> seen = {{1, 10.0}, {2, 20.0}};
    // Current: 1 persists, 2 departs, 3+4 are new.
    std::vector<Phantom> current = {mk(1, 10.0), mk(3, 50.0), mk(4, 51.0)};
    const auto d = phantom_lifecycle_diff(seen, current, /*now=*/100.0);

    REQUIRE(d.new_indices.size() == 2);
    CHECK(d.new_indices[0] == 1u);  // index of phantom id=3
    CHECK(d.new_indices[1] == 2u);  // index of phantom id=4

    REQUIRE(d.departed.size() == 1);
    CHECK(d.departed[0].first  == 2);
    CHECK(d.departed[0].second == doctest::Approx(80.0));

    CHECK(seen.size() == 3);
    CHECK(seen.at(1) == 10.0);
    CHECK(seen.at(3) == 50.0);
    CHECK(seen.at(4) == 51.0);
    CHECK(seen.find(2) == seen.end());
}

TEST_CASE("phantom_lifecycle_diff: spawn_time is what gets stored, not now_ts") {
    std::unordered_map<long long, double> seen;
    std::vector<Phantom> current = {mk(7, /*spawn=*/42.0)};
    phantom_lifecycle_diff(seen, current, /*now=*/9999.0);
    CHECK(seen.at(7) == 42.0);
}

TEST_CASE("phantom_lifecycle_diff: caller-erased ids before the call don't count "
          "as decays (pin path simulation)") {
    // Simulates the pin path: phantom 5 was in seen, gets erased by the
    // pin code, and the SAME frame's current snapshot already had it
    // removed (phantom_buffer.remove). The diff should be silent on id 5.
    std::unordered_map<long long, double> seen = {{5, 10.0}, {6, 11.0}};
    seen.erase(5);  // pin path did this before the diff runs
    std::vector<Phantom> current = {mk(6, 11.0)};
    const auto d = phantom_lifecycle_diff(seen, current, 100.0);
    CHECK(d.new_indices.empty());
    CHECK(d.departed.empty());  // <-- the key invariant: no decay for the pinned id
    CHECK(seen.size() == 1);
}

TEST_CASE("phantom_lifecycle_diff: duplicate ids in current are tolerated -- "
          "first occurrence wins for seen entry, no spurious newness on second") {
    // Defensive: phantom_buffer should never produce duplicates, but if
    // it ever did (e.g. UDP retransmission bug), the diff should not
    // double-fire spawn events. Second occurrence of the same id is
    // already in seen by the time we get to it, so it's not "new" again.
    std::unordered_map<long long, double> seen;
    std::vector<Phantom> current = {mk(9, 1.0), mk(9, 2.0)};
    const auto d = phantom_lifecycle_diff(seen, current, 10.0);
    REQUIRE(d.new_indices.size() == 1);
    CHECK(d.new_indices[0] == 0u);
    CHECK(seen.at(9) == 1.0);  // first-write-wins
}
