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

TEST_CASE("graph_buffer: snapshot is non-destructive (same result on a second call)") {
    GraphBuffer buf;
    buf.publish_positions({{1, 1, 1}, {2, 2, 2}});
    buf.set_edges({{0, 1}});

    std::vector<Vector3> p1, p2;
    std::vector<Edge>    e1, e2;
    buf.snapshot(p1, e1);
    buf.snapshot(p2, e2);

    REQUIRE(p1.size() == 2);
    REQUIRE(p2.size() == 2);
    REQUIRE(e1.size() == 1);
    REQUIRE(e2.size() == 1);
    CHECK(p1[0].x == p2[0].x);
    CHECK(e1[0].source == e2[0].source);
}

TEST_CASE("graph_buffer: publish_positions with empty vector clears the buffer") {
    GraphBuffer buf;
    buf.publish_positions({{1, 1, 1}, {2, 2, 2}});
    buf.publish_positions({});

    std::vector<Vector3> positions;
    std::vector<Edge>    edges;
    buf.snapshot(positions, edges);
    CHECK(positions.empty());
}

TEST_CASE("graph_buffer: set_edges with empty vector clears previous edges") {
    GraphBuffer buf;
    buf.set_edges({{0, 1}, {1, 2}});
    buf.set_edges({});  // explicitly clear

    std::vector<Vector3> positions;
    std::vector<Edge>    edges;
    buf.snapshot(positions, edges);
    CHECK(edges.empty());
}

TEST_CASE("graph_buffer: positions-only snapshot doesn't touch caller's edges vector") {
    GraphBuffer buf;
    buf.publish_positions({{1, 1, 1}, {2, 2, 2}});
    buf.set_edges({{0, 1}, {1, 2}});

    std::vector<Vector3> positions;
    std::vector<Edge>    caller_edges = {{99, 99, "preserved", "test", "confirmed"}};

    buf.snapshot(positions);

    REQUIRE(positions.size() == 2);
    // The positions-only overload must NOT touch caller_edges.
    REQUIRE(caller_edges.size() == 1);
    CHECK(caller_edges[0].source == 99);
    CHECK(caller_edges[0].label  == "preserved");
}

TEST_CASE("graph_buffer: edits to main's edges survive across snapshot calls") {
    // Regression for the bug where operator-edited Edge.label was being
    // overwritten each frame by snapshot(positions, edges). With the
    // positions-only overload, main owns edges authoritatively.
    GraphBuffer buf;
    buf.publish_positions({{0, 0, 0}});
    buf.set_edges({{0, 0, "from-buffer", "knows", "confirmed"}});

    // Simulate main owning its own edges and editing them.
    std::vector<Edge> main_edges = {{0, 0, "from-buffer", "knows", "confirmed"}};
    main_edges[0].label = "operator-edit";
    main_edges[0].kind  = "owns";

    // Repeated positions-only snapshots leave main_edges untouched.
    for (int i = 0; i < 5; ++i) {
        std::vector<Vector3> positions;
        buf.snapshot(positions);
    }
    CHECK(main_edges[0].label == "operator-edit");
    CHECK(main_edges[0].kind  == "owns");
}

TEST_CASE("graph_buffer: snapshot copies (mutating the output doesn't affect the buffer)") {
    GraphBuffer buf;
    buf.publish_positions({{1, 1, 1}, {2, 2, 2}});

    std::vector<Vector3> p1;
    std::vector<Edge>    e1;
    buf.snapshot(p1, e1);
    p1[0].x = 999.0f;  // mutate the snapshot

    std::vector<Vector3> p2;
    std::vector<Edge>    e2;
    buf.snapshot(p2, e2);
    CHECK(p2[0].x == doctest::Approx(1.0f));  // buffer untouched
}

TEST_CASE("graph_buffer: a later set_edges replaces the previous edge list") {
    GraphBuffer buf;
    buf.set_edges({{0, 1}, {1, 2}, {2, 3}});
    buf.set_edges({{9, 9}});

    std::vector<Vector3> positions;
    std::vector<Edge>    edges;
    buf.snapshot(positions, edges);

    REQUIRE(edges.size() == 1);
    CHECK(edges[0].source == 9);
    CHECK(edges[0].target == 9);
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

TEST_CASE("graph_buffer: multiple concurrent readers don't crash or deadlock") {
    // Spin a single producer and several reader threads. Each reader pulls
    // snapshots in a tight loop; the producer keeps rewriting. Verifies the
    // mutex doesn't deadlock under read contention and that snapshots stay
    // internally consistent (all elements equal to the current publish).
    GraphBuffer buf;
    std::atomic<bool> stop{false};

    std::thread producer([&]() {
        const std::vector<Vector3> a(64, Vector3{1, 1, 1});
        const std::vector<Vector3> b(64, Vector3{2, 2, 2});
        bool toggle = false;
        while (!stop.load(std::memory_order_relaxed)) {
            buf.publish_positions(toggle ? b : a);
            toggle = !toggle;
        }
    });

    const auto reader_body = [&]() {
        const auto t0 = std::chrono::steady_clock::now();
        while (std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(120)) {
            std::vector<Vector3> positions;
            std::vector<Edge>    edges;
            buf.snapshot(positions, edges);
            if (!positions.empty()) {
                const float v = positions[0].x;
                REQUIRE((v == 1.0f || v == 2.0f));
                for (const auto& p : positions) {
                    REQUIRE(p.x == v);
                }
            }
        }
    };

    std::thread r1(reader_body);
    std::thread r2(reader_body);
    std::thread r3(reader_body);
    r1.join();
    r2.join();
    r3.join();
    stop.store(true);
    producer.join();
}

TEST_CASE("graph_buffer: multiple concurrent producers don't crash; end state is consistent") {
    // Two producers each repeatedly publish their own distinct vector. The
    // mutex serializes the writes so any observed snapshot must be either
    // all-A or all-B; never a mix. Worth checking because the original
    // tests only exercised a single producer.
    GraphBuffer buf;
    std::atomic<bool> stop{false};

    auto producer = [&](float value) {
        const std::vector<Vector3> v(40, Vector3{value, value, value});
        while (!stop.load(std::memory_order_relaxed)) {
            buf.publish_positions(v);
        }
    };
    std::thread p1(producer, 1.0f);
    std::thread p2(producer, 2.0f);

    const auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(120)) {
        std::vector<Vector3> positions;
        std::vector<Edge>    edges;
        buf.snapshot(positions, edges);
        if (!positions.empty()) {
            REQUIRE(positions.size() == 40);
            const float v = positions[0].x;
            REQUIRE((v == 1.0f || v == 2.0f));
            for (const auto& p : positions) {
                REQUIRE(p.x == v);  // no torn snapshot mixing the two values
            }
        }
    }
    stop.store(true);
    p1.join();
    p2.join();
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
