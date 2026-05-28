#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "app/promote.h"

using zg::app::promote_phantom;
using zg::telemetry::Connection;
using zg::telemetry::Phantom;

namespace {
Phantom make_phantom() {
    Phantom p;
    p.id = 42;
    p.position = {1.0f, 2.0f, 3.0f};
    p.label = "kernel";
    p.content = "the kernel sits between Linus and MINIX";
    p.spawn_time = 100.0;
    p.source = "ollama:llama3.2:3b";
    p.connections = {{1, ""}, {2, ""}, {5, ""}};
    return p;
}
}

TEST_CASE("promote_phantom: node fields copied from phantom") {
    const auto ph = make_phantom();
    const auto out = promote_phantom(ph, /*new_id=*/10, /*now_ts=*/1700000000.0,
                                     /*positions_size=*/20);
    CHECK(out.node.id           == 10);
    CHECK(out.node.position.x   == 1.0f);
    CHECK(out.node.position.y   == 2.0f);
    CHECK(out.node.position.z   == 3.0f);
    CHECK(out.node.title        == "kernel");
    CHECK(out.node.content      == "the kernel sits between Linus and MINIX");
    CHECK(out.node.first_seen   == 1700000000.0);
    CHECK(out.node.last_touched == 1700000000.0);
}

TEST_CASE("promote_phantom: node lands at tier=phantom (trust gradient)") {
    const auto ph = make_phantom();
    const auto out = promote_phantom(ph, 10, 0.0, 20);
    CHECK(out.node.tier == "phantom");
}

TEST_CASE("promote_phantom: edges materialised at certainty=phantom") {
    const auto ph = make_phantom();
    const auto out = promote_phantom(ph, 10, 0.0, 20);
    REQUIRE(out.edges.size() == 3);
    for (const auto& e : out.edges) {
        CHECK(e.source    == 10u);
        CHECK(e.certainty == "phantom");
        CHECK(e.label.empty());
        CHECK(e.kind.empty());
    }
    CHECK(out.edges[0].target == 1u);
    CHECK(out.edges[1].target == 2u);
    CHECK(out.edges[2].target == 5u);
}

TEST_CASE("promote_phantom: empty content carries through (no crash, no synth)") {
    auto ph = make_phantom();
    ph.content.clear();
    const auto out = promote_phantom(ph, 10, 0.0, 20);
    CHECK(out.node.content.empty());
}

TEST_CASE("promote_phantom: out-of-range connection ids are silently dropped") {
    auto ph = make_phantom();
    ph.connections = {{1, ""}, {999, ""}, {2, ""}};  // 999 OOR for positions_size=20
    const auto out = promote_phantom(ph, 10, 0.0, /*positions_size=*/20);
    REQUIRE(out.edges.size() == 2);
    CHECK(out.edges[0].target == 1u);
    CHECK(out.edges[1].target == 2u);
}

TEST_CASE("promote_phantom: negative connection ids are silently dropped") {
    auto ph = make_phantom();
    ph.connections = {{-1, ""}, {3, ""}, {-7, ""}};
    const auto out = promote_phantom(ph, 10, 0.0, 20);
    REQUIRE(out.edges.size() == 1);
    CHECK(out.edges[0].target == 3u);
}

TEST_CASE("promote_phantom: self-edge (connection to new_id) is silently dropped") {
    auto ph = make_phantom();
    ph.connections = {{1, ""}, {10, ""}, {2, ""}};  // 10 is the new node itself
    const auto out = promote_phantom(ph, /*new_id=*/10, 0.0, 20);
    REQUIRE(out.edges.size() == 2);
    CHECK(out.edges[0].target == 1u);
    CHECK(out.edges[1].target == 2u);
}

TEST_CASE("promote_phantom: connection.kind propagates to edge.kind") {
    auto ph = make_phantom();
    ph.connections = {{1, "knows"}, {2, "shell-of"}, {5, "saw-at"}};
    const auto out = promote_phantom(ph, 10, 0.0, 20);
    REQUIRE(out.edges.size() == 3);
    CHECK(out.edges[0].kind == "knows");
    CHECK(out.edges[1].kind == "shell-of");
    CHECK(out.edges[2].kind == "saw-at");
    // certainty STAYS at phantom even when kind is specified -- the kind
    // is a type signal, not a trust signal. The operator promotes both
    // by editing certainty up in the inspector.
    for (const auto& e : out.edges) CHECK(e.certainty == "phantom");
}

TEST_CASE("promote_phantom: empty connection.kind yields empty edge.kind") {
    auto ph = make_phantom();
    ph.connections = {{1, ""}, {2, ""}};
    const auto out = promote_phantom(ph, 10, 0.0, 20);
    REQUIRE(out.edges.size() == 2);
    CHECK(out.edges[0].kind.empty());
    CHECK(out.edges[1].kind.empty());
}

TEST_CASE("promote_phantom: empty connections produces no edges, node still valid") {
    auto ph = make_phantom();
    ph.connections.clear();
    const auto out = promote_phantom(ph, 10, 0.0, 20);
    CHECK(out.edges.empty());
    CHECK(out.node.title == "kernel");
    CHECK(out.node.tier  == "phantom");
}

TEST_CASE("promote_phantom: positions_size=0 means every connection drops") {
    auto ph = make_phantom();
    const auto out = promote_phantom(ph, 10, 0.0, /*positions_size=*/0);
    CHECK(out.edges.empty());
}

TEST_CASE("promote_phantom: new_id=0 (empty graph case) still drops self-edge") {
    auto ph = make_phantom();
    ph.connections = {{0, ""}, {1, ""}, {2, ""}};
    const auto out = promote_phantom(ph, /*new_id=*/0, 0.0, 5);
    // 0 == new_id (self), 1+2 valid -> 2 edges
    REQUIRE(out.edges.size() == 2);
    CHECK(out.edges[0].target == 1u);
    CHECK(out.edges[1].target == 2u);
}
