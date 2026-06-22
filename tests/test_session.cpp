#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "app/session.h"
#include "app/clock.h"

#include <cstddef>

using zg::app::Session;
using zg::persistence::StoredNode;

TEST_CASE("clock: mono_now is positive and monotonic non-decreasing") {
    const double a = zg::app::mono_now();
    const double b = zg::app::mono_now();
    CHECK(a > 0.0);
    CHECK(b >= a);
}

TEST_CASE("session: append_node mints contiguous ids == vector index") {
    Session s;
    CHECK(s.next_node_id() == 0);

    StoredNode a{};
    a.id    = 999;            // a caller-supplied id is ignored
    a.title = "alpha";
    const long long id_a = s.append_node(std::move(a));
    CHECK(id_a == 0);
    REQUIRE(s.stored_nodes.size() == 1);
    CHECK(s.stored_nodes[0].id == 0);            // overwritten to the index
    CHECK(s.stored_nodes[0].title == "alpha");   // other fields preserved
    CHECK(s.next_node_id() == 1);

    StoredNode b{};
    const long long id_b = s.append_node(std::move(b));
    CHECK(id_b == 1);
    CHECK(s.stored_nodes[1].id == 1);
    CHECK(s.next_node_id() == 2);
}

TEST_CASE("session: append_node with null db/physics is a pure vector append") {
    // A default Session has no DB or physics thread; append_node must still
    // assign ids and grow stored_nodes without dereferencing the null deps.
    Session s;
    for (int i = 0; i < 5; ++i) s.append_node(StoredNode{});
    REQUIRE(s.stored_nodes.size() == 5);
    for (std::size_t i = 0; i < 5; ++i) {
        CHECK(s.stored_nodes[i].id == static_cast<long long>(i));
    }
}
