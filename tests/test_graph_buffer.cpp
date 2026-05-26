#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "graph/graph_buffer.h"

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

using zg::graph::GraphBuffer;
using zg::graph::Edge;

TEST_CASE("graph_buffer: snapshot before any publish returns empty vectors") {
    GraphBuffer buf;
    std::vector<Vector3> positions{{99.0f, 99.0f, 99.0f}};  // pre-populated junk
    std::vector<Edge>    edges{{1, 2}};
    buf.snapshot(positions, edges);
    CHECK(positions.empty());
    CHECK(edges.empty());
}

TEST_CASE("graph_buffer: snapshot returns the most recently published positions") {
    GraphBuffer buf;
    buf.publish_positions({{1.0f, 2.0f, 3.0f}, {4.0f, 5.0f, 6.0f}});

    std::vector<Vector3> positions;
    std::vector<Edge>    edges;
    buf.snapshot(positions, edges);

    REQUIRE(positions.size() == 2);
    CHECK(positions[0].x == doctest::Approx(1.0f));
    CHECK(positions[0].y == doctest::Approx(2.0f));
    CHECK(positions[0].z == doctest::Approx(3.0f));
    CHECK(positions[1].x == doctest::Approx(4.0f));
    CHECK(edges.empty());
}

TEST_CASE("graph_buffer: a later publish overwrites the previous one") {
    GraphBuffer buf;
    buf.publish_positions({{1, 1, 1}, {2, 2, 2}, {3, 3, 3}});
    buf.publish_positions({{9, 9, 9}});

    std::vector<Vector3> positions;
    std::vector<Edge>    edges;
    buf.snapshot(positions, edges);

    REQUIRE(positions.size() == 1);
    CHECK(positions[0].x == doctest::Approx(9.0f));
}

TEST_CASE("graph_buffer: set_edges is independent of positions") {
    GraphBuffer buf;
    buf.set_edges({{0, 1}, {1, 2}, {2, 3}});

    std::vector<Vector3> positions;
    std::vector<Edge>    edges;
    buf.snapshot(positions, edges);

    CHECK(positions.empty());
    REQUIRE(edges.size() == 3);
    CHECK(edges[0].source == 0);
    CHECK(edges[2].target == 3);
}

TEST_CASE("graph_buffer: edges and positions can be set in either order") {
    GraphBuffer buf;
    buf.publish_positions({{1, 1, 1}});
    buf.set_edges({{0, 0}});

    std::vector<Vector3> positions;
    std::vector<Edge>    edges;
    buf.snapshot(positions, edges);

    REQUIRE(positions.size() == 1);
    REQUIRE(edges.size() == 1);
}

TEST_CASE("graph_buffer: producer/consumer hammering doesn't tear a snapshot") {
    // Verifies the mutex actually guards against torn reads: a snapshot must
    // observe either the empty initial state OR a fully-formed vector where
    // every element is {1, 1, 1}. A torn snapshot (some-elements-old,
    // some-new, or partially-sized) would be detected here.
    GraphBuffer buf;
    std::atomic<bool>  stop{false};
    std::atomic<bool>  saw_a_populated_snapshot{false};

    std::thread producer([&]() {
        const std::vector<Vector3> ones(100, Vector3{1.0f, 1.0f, 1.0f});
        const std::vector<Vector3> twos(100, Vector3{2.0f, 2.0f, 2.0f});
        bool toggle = false;
        while (!stop.load(std::memory_order_relaxed)) {
            buf.publish_positions(toggle ? twos : ones);
            toggle = !toggle;
        }
    });

    const auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(150)) {
        std::vector<Vector3> positions;
        std::vector<Edge>    edges;
        buf.snapshot(positions, edges);
        if (!positions.empty()) {
            saw_a_populated_snapshot = true;
            REQUIRE(positions.size() == 100);
            const float v = positions[0].x;
            REQUIRE((v == 1.0f || v == 2.0f));
            for (const auto& p : positions) {
                REQUIRE(p.x == v);
                REQUIRE(p.y == v);
                REQUIRE(p.z == v);
            }
        }
    }

    stop.store(true);
    producer.join();

    CHECK(saw_a_populated_snapshot.load());
}
