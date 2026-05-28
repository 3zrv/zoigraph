#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "render/labels.h"

using zg::render::compute_label_set;
using zg::graph::Edge;
using zg::telemetry::Connection;
using zg::telemetry::Phantom;
using zg::persistence::StoredNode;

namespace {
StoredNode mk(long long id, bool deleted = false) {
    StoredNode n{};
    n.id      = id;
    n.title   = "n" + std::to_string(id);
    n.deleted = deleted;
    return n;
}
}

TEST_CASE("compute_label_set: empty inputs return empty set") {
    std::vector<StoredNode> nodes = {mk(0), mk(1), mk(2)};
    const auto s = compute_label_set(/*selected=*/-1, {}, {}, {}, nodes, false);
    CHECK(s.empty());
}

TEST_CASE("compute_label_set: selected only adds the selected index") {
    std::vector<StoredNode> nodes = {mk(0), mk(1), mk(2)};
    const auto s = compute_label_set(1, {}, {}, {}, nodes, false);
    CHECK(s == std::set<std::size_t>{1});
}

TEST_CASE("compute_label_set: selected + edges pull in 1-hop neighbours in both directions") {
    std::vector<StoredNode> nodes = {mk(0), mk(1), mk(2), mk(3)};
    std::vector<Edge> edges = {
        {0, 1},   // 0 -> 1 (source==selected brings 1)
        {1, 2},   // unrelated to selected, ignored
        {3, 0},   // 3 -> 0 (target==selected brings 3)
    };
    const auto s = compute_label_set(/*selected=*/0, edges, {}, {}, nodes, false);
    CHECK(s == std::set<std::size_t>{0, 1, 3});
}

TEST_CASE("compute_label_set: phantom connection targets are added") {
    std::vector<StoredNode> nodes = {mk(0), mk(1), mk(2), mk(3)};
    Phantom ph;
    ph.connections = {Connection{1, ""}, Connection{2, "knows"}};
    const auto s = compute_label_set(-1, {}, {ph}, {}, nodes, false);
    CHECK(s == std::set<std::size_t>{1, 2});
}

TEST_CASE("compute_label_set: bones chosen indices are included") {
    std::vector<StoredNode> nodes = {mk(0), mk(1), mk(2), mk(3)};
    const auto s = compute_label_set(-1, {}, {}, /*bones=*/{1, 3}, nodes, false);
    CHECK(s == std::set<std::size_t>{1, 3});
}

TEST_CASE("compute_label_set: all sources unioned + deduped") {
    std::vector<StoredNode> nodes = {mk(0), mk(1), mk(2), mk(3)};
    std::vector<Edge> edges = {{0, 1}};                // pulls in 1 via selected=0
    Phantom ph;
    ph.connections = {Connection{1, ""}, Connection{2, ""}};  // 1 dup, 2 new
    std::vector<std::size_t> bones = {2, 3};                  // 2 dup, 3 new
    const auto s = compute_label_set(/*selected=*/0, edges, {ph}, bones, nodes, false);
    CHECK(s == std::set<std::size_t>{0, 1, 2, 3});
}

TEST_CASE("compute_label_set: tombstoned indices are dropped from every source") {
    std::vector<StoredNode> nodes = {
        mk(0),
        mk(1, /*deleted=*/true),
        mk(2, /*deleted=*/true),
        mk(3),
    };
    std::vector<Edge> edges = {{0, 1}};               // 1 dead -> skip
    Phantom ph; ph.connections = {Connection{2, ""}}; // 2 dead -> skip
    std::vector<std::size_t> bones = {1};             // 1 dead -> skip
    const auto s = compute_label_set(/*selected=*/0, edges, {ph}, bones, nodes, false);
    CHECK(s == std::set<std::size_t>{0});
}

TEST_CASE("compute_label_set: out-of-range indices are dropped silently") {
    std::vector<StoredNode> nodes = {mk(0), mk(1)};
    std::vector<Edge> edges = {{0, 99}};               // 99 OOR
    Phantom ph; ph.connections = {Connection{77, ""}}; // 77 OOR
    std::vector<std::size_t> bones = {88};             // 88 OOR
    // selected also OOR -> contributes nothing
    const auto s = compute_label_set(/*selected=*/55, edges, {ph}, bones, nodes, false);
    CHECK(s.empty());
}

TEST_CASE("compute_label_set: negative phantom target ids are dropped") {
    std::vector<StoredNode> nodes = {mk(0), mk(1), mk(2)};
    Phantom ph;
    ph.connections = {Connection{-1, ""}, Connection{1, ""}, Connection{-99, ""}};
    const auto s = compute_label_set(-1, {}, {ph}, {}, nodes, false);
    CHECK(s == std::set<std::size_t>{1});
}

TEST_CASE("compute_label_set: always_show returns every non-deleted index") {
    std::vector<StoredNode> nodes = {
        mk(0),
        mk(1, /*deleted=*/true),
        mk(2),
        mk(3, /*deleted=*/true),
        mk(4),
    };
    const auto s = compute_label_set(-1, {}, {}, {}, nodes, /*always_show=*/true);
    CHECK(s == std::set<std::size_t>{0, 2, 4});
}

TEST_CASE("compute_label_set: always_show ignores selected/phantoms/bones (they're a subset)") {
    std::vector<StoredNode> nodes = {mk(0), mk(1), mk(2)};
    Phantom ph; ph.connections = {Connection{0, ""}};
    const auto s = compute_label_set(
        /*selected=*/1, /*edges=*/{}, /*phantoms=*/{ph}, /*bones=*/{2},
        nodes, /*always_show=*/true);
    CHECK(s == std::set<std::size_t>{0, 1, 2});
}
