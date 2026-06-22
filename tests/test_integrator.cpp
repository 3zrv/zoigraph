#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "physics/physics_thread.h"
#include "graph/graph_buffer.h"

#include <chrono>
#include <cmath>
#include <random>
#include <thread>
#include <vector>

using zg::physics::SimParams;
using zg::physics::integrate_step;
using zg::physics::PhysicsThread;
using zg::graph::Edge;
using zg::graph::GraphBuffer;

namespace {

// All-zero forces: turn off every interaction so each test isolates one term.
SimParams zero_forces() {
    SimParams p{};
    p.repulsion_k = 0.0f;
    p.spring_k    = 0.0f;
    p.center_k    = 0.0f;
    return p;
}

float distance(Vector3 a, Vector3 b) {
    const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return std::sqrt(dx*dx + dy*dy + dz*dz);
}

}  // namespace

TEST_CASE("integrator: damping alone decays a moving node's velocity to ~zero") {
    std::vector<Vector3> positions  = {{0, 0, 0}};
    std::vector<Vector3> velocities = {{5.0f, -3.0f, 2.0f}};
    SimParams params = zero_forces();

    for (int i = 0; i < 200; ++i) {
        integrate_step(positions, velocities, {}, params);
    }

    // damping^200 with damping=0.85 is ≈ 10^-14, well under any test threshold.
    CHECK(std::fabs(velocities[0].x) < 1e-3f);
    CHECK(std::fabs(velocities[0].y) < 1e-3f);
    CHECK(std::fabs(velocities[0].z) < 1e-3f);
}

TEST_CASE("integrator: centering pulls an off-origin node toward origin") {
    std::vector<Vector3> positions  = {{10.0f, 0.0f, 0.0f}};
    std::vector<Vector3> velocities = {{0.0f, 0.0f, 0.0f}};
    SimParams params = zero_forces();
    params.center_k = 0.5f;

    const float start_radius = distance(positions[0], {0, 0, 0});
    for (int i = 0; i < 200; ++i) {
        integrate_step(positions, velocities, {}, params);
    }
    const float end_radius = distance(positions[0], {0, 0, 0});

    // No anti-force exists; the system is overdamped, so the node must
    // strictly approach the origin without overshooting back outward.
    CHECK(end_radius < start_radius / 2.0f);
}

TEST_CASE("integrator: coulomb separates two close-but-distinct nodes") {
    // Coincident positions produce a zero direction vector by design; the
    // simulation relies on random initialization to avoid that degenerate
    // case. Here we mirror reality with a small offset.
    std::vector<Vector3> positions  = {{0.1f, 0, 0}, {-0.1f, 0, 0}};
    std::vector<Vector3> velocities = {{0, 0, 0}, {0, 0, 0}};
    SimParams params = zero_forces();
    params.repulsion_k = 80.0f;

    const float start = distance(positions[0], positions[1]);
    for (int i = 0; i < 20; ++i) {
        integrate_step(positions, velocities, {}, params);
    }
    CHECK(distance(positions[0], positions[1]) > start);
}

TEST_CASE("integrator: spring at rest length yields a stable equilibrium") {
    SimParams params = zero_forces();
    params.spring_k    = 0.5f;
    params.spring_rest = 4.0f;

    std::vector<Vector3> positions  = {{-2.0f, 0, 0}, {2.0f, 0, 0}};  // exactly at rest length
    std::vector<Vector3> velocities = {{0, 0, 0}, {0, 0, 0}};
    const std::vector<Edge> edges   = {{0, 1}};

    for (int i = 0; i < 100; ++i) {
        integrate_step(positions, velocities, edges, params);
    }

    // No external forces, started at rest length, started at rest. Should
    // still be at rest length (within float epsilon × loop count).
    CHECK(distance(positions[0], positions[1]) == doctest::Approx(4.0f).epsilon(0.001f));
}

TEST_CASE("integrator: stretched spring contracts toward rest length over time") {
    SimParams params = zero_forces();
    params.spring_k    = 0.1f;
    params.spring_rest = 4.0f;

    std::vector<Vector3> positions  = {{-10.0f, 0, 0}, {10.0f, 0, 0}};  // stretched
    std::vector<Vector3> velocities = {{0, 0, 0}, {0, 0, 0}};
    const std::vector<Edge> edges   = {{0, 1}};

    const float start = distance(positions[0], positions[1]);
    for (int i = 0; i < 500; ++i) {
        integrate_step(positions, velocities, edges, params);
    }
    const float end = distance(positions[0], positions[1]);

    CHECK(end < start);          // contracted
    CHECK(end > 1.0f);           // didn't overshoot through each other
}

TEST_CASE("integrator: velocity clamp caps speed even under extreme repulsion") {
    SimParams params = zero_forces();
    params.repulsion_k = 1.0e6f;   // absurdly large to force the clamp
    params.max_speed   = 20.0f;
    params.damping     = 1.0f;     // no damping so we see the raw clamp

    std::vector<Vector3> positions  = {{0, 0, 0}, {0.001f, 0, 0}};  // nearly overlapping
    std::vector<Vector3> velocities = {{0, 0, 0}, {0, 0, 0}};

    for (int i = 0; i < 5; ++i) {
        integrate_step(positions, velocities, {}, params);
    }

    for (const auto& v : velocities) {
        const float speed = std::sqrt(v.x*v.x + v.y*v.y + v.z*v.z);
        CHECK(speed <= params.max_speed + 1e-3f);
    }
}

TEST_CASE("integrator: phantom_repulsion_k = 0 disables phantom shoves") {
    // Even with phantoms present, a zero coefficient turns them into no-ops.
    SimParams params = zero_forces();
    params.phantom_repulsion_k = 0.0f;

    std::vector<Vector3> positions  = {{0, 0, 0}};
    std::vector<Vector3> velocities = {{0, 0, 0}};
    const std::vector<Vector3> phantoms = {{1.0f, 0, 0}, {0, 1.0f, 0}};

    for (int i = 0; i < 30; ++i) {
        integrate_step(positions, velocities, {}, params, phantoms);
    }
    CHECK(positions[0].x == doctest::Approx(0.0f).epsilon(1e-5));
    CHECK(positions[0].y == doctest::Approx(0.0f).epsilon(1e-5));
}

TEST_CASE("integrator: a phantom shoves a nearby static node away") {
    // Per directive §5.B, phantoms apply one-way Coulomb repulsion with a
    // much larger coefficient than static-vs-static. A static node placed
    // at the origin with a phantom just to its right should drift to the
    // left over a handful of ticks.
    std::vector<Vector3> positions  = {{0.0f, 0.0f, 0.0f}};
    std::vector<Vector3> velocities = {{0.0f, 0.0f, 0.0f}};
    SimParams params = zero_forces();
    params.phantom_repulsion_k = 800.0f;

    const std::vector<Vector3> phantoms = {{2.0f, 0.0f, 0.0f}};
    for (int i = 0; i < 30; ++i) {
        integrate_step(positions, velocities, {}, params, phantoms);
    }

    CHECK(positions[0].x < -0.1f);                   // drifted in -x direction
    CHECK(std::fabs(positions[0].y) < 1e-3f);        // no spurious y motion
    CHECK(std::fabs(positions[0].z) < 1e-3f);        // no spurious z motion
}

TEST_CASE("integrator: phantoms do not accumulate reaction forces themselves") {
    // The vector<Vector3> of phantom positions is by-value into the function;
    // there's no way for the integrator to modify the caller's phantom list.
    // Verify by holding a reference to the snapshot the integrator sees and
    // confirming the static node moves while phantoms are unchanged.
    std::vector<Vector3> positions  = {{0, 0, 0}};
    std::vector<Vector3> velocities = {{0, 0, 0}};
    SimParams params = zero_forces();
    params.phantom_repulsion_k = 800.0f;

    std::vector<Vector3> phantoms = {{3.0f, 0.0f, 0.0f}};
    const Vector3 phantom_before = phantoms[0];

    for (int i = 0; i < 30; ++i) {
        integrate_step(positions, velocities, {}, params, phantoms);
    }

    CHECK(phantoms[0].x == phantom_before.x);
    CHECK(phantoms[0].y == phantom_before.y);
    CHECK(phantoms[0].z == phantom_before.z);
    CHECK(positions[0].x < 0.0f);  // static node still moved
}

TEST_CASE("integrator: 50 randomly-seeded nodes never exceed max_speed under heavy repulsion") {
    // Stress test: many nodes packed close together with strong repulsion.
    // The velocity clamp must keep every component within max_speed even
    // when the integrator sees huge forces in early ticks.
    std::mt19937 rng(42);
    std::uniform_real_distribution<float> d(-2.0f, 2.0f);
    std::vector<Vector3> positions, velocities;
    for (int i = 0; i < 50; ++i) {
        positions.push_back({d(rng), d(rng), d(rng)});
        velocities.push_back({0, 0, 0});
    }
    SimParams params{};
    params.repulsion_k = 200.0f;
    params.max_speed   = 20.0f;
    params.damping     = 0.95f;
    params.dt          = 0.05f;

    for (int t = 0; t < 50; ++t) {
        integrate_step(positions, velocities, {}, params);
    }
    for (const auto& v : velocities) {
        const float speed = std::sqrt(v.x*v.x + v.y*v.y + v.z*v.z);
        CHECK(speed <= params.max_speed + 1e-3f);
    }
}

TEST_CASE("integrator: damping = 1.0 preserves velocity indefinitely under no forces") {
    std::vector<Vector3> positions  = {{0, 0, 0}};
    std::vector<Vector3> velocities = {{2.0f, 0, 0}};
    SimParams params = zero_forces();
    params.damping = 1.0f;

    for (int i = 0; i < 50; ++i) {
        integrate_step(positions, velocities, {}, params);
    }
    // No damping, no force: velocity stays exactly 2.0 (modulo float drift).
    CHECK(velocities[0].x == doctest::Approx(2.0f));
    // Position drifts at v*dt per tick; 50 ticks at dt=0.05 = 2.5s, displacement 5.0.
    CHECK(positions[0].x == doctest::Approx(5.0f).epsilon(0.01f));
}

TEST_CASE("integrator: zero-velocity node with no forces does not drift") {
    std::vector<Vector3> positions  = {{1.0f, 2.0f, 3.0f}};
    std::vector<Vector3> velocities = {{0, 0, 0}};
    SimParams params = zero_forces();
    params.damping = 0.99f;

    for (int i = 0; i < 50; ++i) {
        integrate_step(positions, velocities, {}, params);
    }

    CHECK(positions[0].x == doctest::Approx(1.0f));
    CHECK(positions[0].y == doctest::Approx(2.0f));
    CHECK(positions[0].z == doctest::Approx(3.0f));
}

TEST_CASE("integrator: connected pair reaches a spring/repulsion equilibrium") {
    // Two nodes joined by one edge with both Coulomb repulsion AND Hooke
    // attraction active. They should settle into a stable separation neither
    // at the bare rest length nor compressed to zero — somewhere between.
    SimParams params = zero_forces();
    params.spring_k    = 0.3f;
    params.spring_rest = 4.0f;
    params.repulsion_k = 5.0f;

    std::vector<Vector3> positions  = {{-2.0f, 0, 0}, {2.0f, 0, 0}};
    std::vector<Vector3> velocities = {{0, 0, 0}, {0, 0, 0}};
    const std::vector<Edge> edges   = {{0, 1}};

    for (int i = 0; i < 500; ++i) {
        integrate_step(positions, velocities, edges, params);
    }

    const float final_sep = distance(positions[0], positions[1]);
    // Final separation should be larger than rest length (repulsion pushes
    // them apart) but not blown up.
    CHECK(final_sep > 4.0f);
    CHECK(final_sep < 8.0f);
}

TEST_CASE("integrator: multiple phantoms compound force on a single static node") {
    // Two phantoms symmetric in y bracketing a static node at origin should
    // cancel out in y but push the node in the -x direction since both sit
    // at +x. (Net force is the vector sum.)
    SimParams params = zero_forces();
    params.phantom_repulsion_k = 400.0f;

    std::vector<Vector3> positions  = {{0, 0, 0}};
    std::vector<Vector3> velocities = {{0, 0, 0}};
    const std::vector<Vector3> phantoms = {
        {3.0f,  2.0f, 0},
        {3.0f, -2.0f, 0},
    };

    for (int i = 0; i < 30; ++i) {
        integrate_step(positions, velocities, {}, params, phantoms);
    }

    CHECK(positions[0].x < -0.1f);
    CHECK(std::fabs(positions[0].y) < 0.05f);  // symmetric phantoms cancel in y
    CHECK(std::fabs(positions[0].z) < 1e-3f);
}

TEST_CASE("integrator: centering pulls a uniform cluster toward origin together") {
    // 5 nodes evenly spaced along +x, no repulsion/springs, only centering.
    // The cluster's centroid should approach origin without dispersing.
    SimParams params = zero_forces();
    params.center_k = 0.4f;

    std::vector<Vector3> positions  = {
        {6, 0, 0}, {7, 0, 0}, {8, 0, 0}, {9, 0, 0}, {10, 0, 0},
    };
    std::vector<Vector3> velocities(positions.size(), Vector3{0, 0, 0});

    auto centroid_x = [&]() {
        float s = 0.0f;
        for (const auto& p : positions) s += p.x;
        return s / positions.size();
    };
    const float start_centroid = centroid_x();

    for (int i = 0; i < 200; ++i) {
        integrate_step(positions, velocities, {}, params);
    }

    const float end_centroid = centroid_x();
    CHECK(end_centroid < start_centroid);
    CHECK(std::fabs(end_centroid) < std::fabs(start_centroid));
}

TEST_CASE("integrator: three-node spring chain settles toward a line") {
    // 0 -- 1 -- 2  (springs only, no other forces). Starting from a wiggly
    // arrangement, the chain should end up roughly colinear in x with
    // separations near rest_length each.
    SimParams params = zero_forces();
    params.spring_k    = 0.5f;
    params.spring_rest = 3.0f;

    std::vector<Vector3> positions  = {{-5, 0, 0}, {0, 2, 1}, {6, -1, 0}};
    std::vector<Vector3> velocities(3, Vector3{0, 0, 0});
    const std::vector<Edge> edges = {{0, 1}, {1, 2}};

    for (int i = 0; i < 1000; ++i) {
        integrate_step(positions, velocities, edges, params);
    }

    const float d01 = distance(positions[0], positions[1]);
    const float d12 = distance(positions[1], positions[2]);
    // Each spring should be near rest length.
    CHECK(std::fabs(d01 - 3.0f) < 0.3f);
    CHECK(std::fabs(d12 - 3.0f) < 0.3f);
}

TEST_CASE("integrator: max_speed = 0 hard-clamps every velocity to zero") {
    SimParams params = zero_forces();
    params.repulsion_k = 1000.0f;
    params.damping     = 1.0f;
    params.max_speed   = 0.0f;

    std::vector<Vector3> positions  = {{0, 0, 0}, {0.05f, 0, 0}};
    std::vector<Vector3> velocities = {{0, 0, 0}, {0, 0, 0}};
    for (int i = 0; i < 10; ++i) {
        integrate_step(positions, velocities, {}, params);
    }
    for (const auto& v : velocities) {
        CHECK(v.x == doctest::Approx(0.0f));
        CHECK(v.y == doctest::Approx(0.0f));
        CHECK(v.z == doctest::Approx(0.0f));
    }
}

TEST_CASE("integrator: empty positions / edges / phantoms is a safe no-op") {
    std::vector<Vector3> positions;
    std::vector<Vector3> velocities;
    SimParams params = zero_forces();
    integrate_step(positions, velocities, {}, params);
    integrate_step(positions, velocities, {}, params, {});
    CHECK(positions.empty());
    CHECK(velocities.empty());
}

TEST_CASE("PhysicsThread: start and stop are idempotent") {
    GraphBuffer buffer;
    PhysicsThread physics({{0, 0, 0}}, {}, buffer, nullptr);

    physics.start();
    physics.start();  // second call is a no-op, no double-spawn
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    physics.stop();
    physics.stop();   // second call is a no-op, no double-join

    // Re-entering after a full stop should also work cleanly.
    physics.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    physics.stop();
}

TEST_CASE("PhysicsThread: hundred enqueue_node calls in a burst all land") {
    GraphBuffer buffer;
    PhysicsThread physics({{0, 0, 0}}, {}, buffer, nullptr);
    physics.start();

    for (int i = 0; i < 100; ++i) {
        physics.enqueue_node({static_cast<float>(i), 0.0f, 0.0f});
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(80));

    std::vector<Vector3> positions;
    std::vector<Edge>    edges;
    buffer.snapshot(positions, edges);
    physics.stop();

    CHECK(positions.size() == 101);
}

TEST_CASE("PhysicsThread: enqueue_node before start() is picked up at first tick") {
    GraphBuffer buffer;
    PhysicsThread physics({{0, 0, 0}}, {}, buffer, nullptr);

    physics.enqueue_node({7.0f, 0.0f, 0.0f});
    physics.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(80));

    std::vector<Vector3> positions;
    std::vector<Edge>    edges;
    buffer.snapshot(positions, edges);
    physics.stop();

    REQUIRE(positions.size() == 2);
    CHECK(std::fabs(positions[1].x - 7.0f) < 2.0f);
}

TEST_CASE("PhysicsThread: empty initial positions doesn't crash and accepts enqueues") {
    GraphBuffer buffer;
    PhysicsThread physics({}, {}, buffer, nullptr);
    physics.start();

    physics.enqueue_node({3.0f, 0.0f, 0.0f});
    physics.enqueue_node({0.0f, 3.0f, 0.0f});
    std::this_thread::sleep_for(std::chrono::milliseconds(80));

    std::vector<Vector3> positions;
    std::vector<Edge>    edges;
    buffer.snapshot(positions, edges);
    physics.stop();

    CHECK(positions.size() == 2);
}

TEST_CASE("PhysicsThread: enqueue_node is drained on the next tick") {
    // Click-to-pin path: a phantom promoted to Static Node calls
    // enqueue_node(position) on the running physics thread, which must
    // pick it up at the start of the next step. Verifies the queue is
    // drained and the new node is reflected in the published snapshot.
    GraphBuffer buffer;
    PhysicsThread physics({{0.0f, 0.0f, 0.0f}}, {}, buffer, /*phantom_buffer=*/nullptr);
    physics.start();

    physics.enqueue_node({5.0f, 5.0f, 5.0f});
    // The thread ticks at ~120 Hz; 80 ms is many ticks worth of margin.
    std::this_thread::sleep_for(std::chrono::milliseconds(80));

    std::vector<Vector3> positions;
    std::vector<Edge>    edges;
    buffer.snapshot(positions, edges);

    physics.stop();

    REQUIRE(positions.size() == 2);
    // The new node has had a few ticks of integration but should still be
    // within striking distance of the original spawn point.
    CHECK(std::fabs(positions[1].x - 5.0f) < 2.0f);
    CHECK(std::fabs(positions[1].y - 5.0f) < 2.0f);
    CHECK(std::fabs(positions[1].z - 5.0f) < 2.0f);
}

TEST_CASE("integrator: out-of-bounds edges are silently skipped (no crash)") {
    // A DB loaded with orphan edges (target references a node that was
    // never re-saved) should not crash the integrator; the bad edge is
    // dropped and the rest of the simulation proceeds.
    std::vector<Vector3> positions  = {{0, 0, 0}, {2, 0, 0}};
    std::vector<Vector3> velocities = {{0, 0, 0}, {0, 0, 0}};
    SimParams params = zero_forces();
    params.spring_k = 0.5f;
    params.spring_rest = 2.0f;
    const std::vector<Edge> edges = {
        {0, 1},          // valid
        {0, 999},        // orphan: out of bounds
        {500, 1000},     // both out of bounds
        {1, 0},          // valid
    };

    integrate_step(positions, velocities, edges, params);
    integrate_step(positions, velocities, edges, params);

    // No out-of-bounds read happened; positions remain finite.
    for (const auto& p : positions) {
        CHECK_FALSE(std::isnan(p.x));
        CHECK_FALSE(std::isnan(p.y));
        CHECK_FALSE(std::isnan(p.z));
    }
}

TEST_CASE("PhysicsThread: enqueue_edge before start() lands on the first tick") {
    GraphBuffer buffer;
    PhysicsThread physics({{0, 0, 0}, {5, 0, 0}}, {}, buffer, nullptr);
    physics.enqueue_edge({0, 1});
    physics.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(80));

    std::vector<Vector3> positions;
    std::vector<Edge>    edges;
    buffer.snapshot(positions, edges);
    physics.stop();

    REQUIRE(edges.size() == 1);
    CHECK(edges[0].source == 0);
    CHECK(edges[0].target == 1);
}

TEST_CASE("PhysicsThread: set_use_barnes_hut toggles cleanly at runtime") {
    GraphBuffer buffer;
    // Start with BH off.
    SimParams p{};
    p.use_barnes_hut = false;
    PhysicsThread physics({{0, 0, 0}, {1, 0, 0}, {-1, 0, 0}}, {}, buffer, nullptr, p);
    physics.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    CHECK_FALSE(physics.use_barnes_hut());
    physics.set_use_barnes_hut(true);
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    CHECK(physics.use_barnes_hut());

    physics.set_use_barnes_hut(false);
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    CHECK_FALSE(physics.use_barnes_hut());

    // The simulation is still alive after multiple toggles.
    std::vector<Vector3> positions;
    std::vector<Edge>    edges;
    buffer.snapshot(positions, edges);
    physics.stop();
    REQUIRE(positions.size() == 3);
}

TEST_CASE("PhysicsThread: set_pin freezes a node at its anchor across many ticks") {
    // Two nodes near each other with strong repulsion. The pinned one
    // should not move; the other should be shoved away.
    GraphBuffer buffer;
    SimParams p{};
    p.repulsion_k = 100.0f;
    PhysicsThread physics({{0.0f, 0.0f, 0.0f}, {0.1f, 0.0f, 0.0f}}, {}, buffer, nullptr, p);
    physics.set_pin(0, {0.0f, 0.0f, 0.0f});
    physics.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    std::vector<Vector3> positions;
    std::vector<Edge>    edges;
    buffer.snapshot(positions);
    physics.stop();

    REQUIRE(positions.size() == 2);
    // Pinned node sits at the anchor.
    CHECK(positions[0].x == doctest::Approx(0.0f).epsilon(0.001f));
    CHECK(positions[0].y == doctest::Approx(0.0f).epsilon(0.001f));
    CHECK(positions[0].z == doctest::Approx(0.0f).epsilon(0.001f));
    // Other node was shoved away.
    CHECK(std::fabs(positions[1].x) > 0.5f);
}

TEST_CASE("PhysicsThread: clear_pin lets a previously-pinned node move again") {
    GraphBuffer buffer;
    SimParams p{};
    p.repulsion_k = 100.0f;
    PhysicsThread physics({{0.0f, 0.0f, 0.0f}, {0.1f, 0.0f, 0.0f}}, {}, buffer, nullptr, p);
    physics.set_pin(0, {0.0f, 0.0f, 0.0f});
    physics.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    physics.clear_pin(0);
    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    std::vector<Vector3> positions;
    std::vector<Edge>    edges;
    buffer.snapshot(positions);
    physics.stop();

    REQUIRE(positions.size() == 2);
    // After clearing the pin, the formerly-pinned node has had time to
    // drift under continued repulsion from the other.
    CHECK(std::fabs(positions[0].x) > 1e-3f);
}

TEST_CASE("PhysicsThread: set_pin with out-of-bounds index is a silent no-op") {
    GraphBuffer buffer;
    PhysicsThread physics({{0, 0, 0}}, {}, buffer, nullptr);
    physics.set_pin(999, {7.0f, 7.0f, 7.0f});  // node doesn't exist
    physics.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(40));

    std::vector<Vector3> positions;
    std::vector<Edge>    edges;
    buffer.snapshot(positions);
    physics.stop();
    REQUIRE(positions.size() == 1);  // no crash, no spurious additions
}

TEST_CASE("PhysicsThread: enqueue_edge appears in the buffer's edge snapshot") {
    // After enqueue_edge drains on the next tick, the physics thread
    // republishes edges_ to the buffer so the render-side snapshot picks
    // them up without main needing to manually set_edges.
    GraphBuffer buffer;
    PhysicsThread physics({{0, 0, 0}, {5, 0, 0}}, {}, buffer, nullptr);
    physics.start();

    physics.enqueue_edge({0, 1});
    std::this_thread::sleep_for(std::chrono::milliseconds(80));

    std::vector<Vector3> positions;
    std::vector<Edge>    edges;
    buffer.snapshot(positions, edges);
    physics.stop();

    REQUIRE(edges.size() == 1);
    CHECK(edges[0].source == 0);
    CHECK(edges[0].target == 1);
}

TEST_CASE("PhysicsThread: many enqueue_edge calls all land via the buffer") {
    GraphBuffer buffer;
    PhysicsThread physics({{0, 0, 0}, {1, 0, 0}, {2, 0, 0}, {3, 0, 0}},
                          {}, buffer, nullptr);
    physics.start();
    physics.enqueue_edge({0, 1});
    physics.enqueue_edge({1, 2});
    physics.enqueue_edge({2, 3});
    physics.enqueue_edge({0, 3});
    std::this_thread::sleep_for(std::chrono::milliseconds(80));

    std::vector<Vector3> positions;
    std::vector<Edge>    edges;
    buffer.snapshot(positions, edges);
    physics.stop();

    CHECK(edges.size() == 4);
}

TEST_CASE("PhysicsThread: enqueue_node + enqueue_edge in one click batch") {
    // Mirrors the click-to-pin flow: a new node and several edges referencing
    // its index get pushed at the same time. Both must land together so the
    // edges' source index resolves to a real position by the next tick.
    GraphBuffer buffer;
    PhysicsThread physics({{0, 0, 0}, {5, 0, 0}}, {}, buffer, nullptr);
    physics.start();

    physics.enqueue_node({10, 0, 0});
    physics.enqueue_edge({2, 0});
    physics.enqueue_edge({2, 1});
    std::this_thread::sleep_for(std::chrono::milliseconds(80));

    std::vector<Vector3> positions;
    std::vector<Edge>    edges;
    buffer.snapshot(positions, edges);
    physics.stop();

    REQUIRE(positions.size() == 3);
    REQUIRE(edges.size() == 2);
    CHECK(edges[0].source == 2);
    CHECK(edges[1].source == 2);
}

TEST_CASE("PhysicsThread: multiple enqueue_node calls all land") {
    GraphBuffer buffer;
    PhysicsThread physics({{0.0f, 0.0f, 0.0f}}, {}, buffer, nullptr);
    physics.start();

    physics.enqueue_node({1, 0, 0});
    physics.enqueue_node({2, 0, 0});
    physics.enqueue_node({3, 0, 0});
    std::this_thread::sleep_for(std::chrono::milliseconds(80));

    std::vector<Vector3> positions;
    std::vector<Edge>    edges;
    buffer.snapshot(positions, edges);
    physics.stop();

    CHECK(positions.size() == 4);
}

TEST_CASE("integrator: every-coefficient-zero params yields zero motion") {
    SimParams params{};
    params.repulsion_k         = 0.0f;
    params.spring_k            = 0.0f;
    params.center_k            = 0.0f;
    params.phantom_repulsion_k = 0.0f;
    params.damping             = 1.0f;
    params.dt                  = 0.05f;
    params.max_speed           = 100.0f;

    std::vector<Vector3> positions  = {{1, 2, 3}, {-4, -5, -6}};
    std::vector<Vector3> velocities = {{0, 0, 0}, {0, 0, 0}};
    const std::vector<Vector3> ghosts = {{0, 0, 0}};

    for (int i = 0; i < 100; ++i) {
        integrate_step(positions, velocities, {{0, 1}}, params, ghosts);
    }

    CHECK(positions[0].x == doctest::Approx(1.0f));
    CHECK(positions[1].z == doctest::Approx(-6.0f));
}

TEST_CASE("integrate: a disabled node is frozen with zero velocity") {
    for (bool bh : {false, true}) {
        std::vector<Vector3> pos = {{-2, 0, 0}, {2, 0, 0}, {0, 3, 0}};
        std::vector<Vector3> vel(3, Vector3{0, 0, 0});
        SimParams p{};
        p.use_barnes_hut = bh;
        const std::vector<char> disabled = {0, 1, 0};  // node 1 disabled
        const Vector3 before = pos[1];
        integrate_step(pos, vel, {}, p, {}, disabled);

        CHECK(pos[1].x == before.x);   // frozen exactly
        CHECK(pos[1].y == before.y);
        CHECK(pos[1].z == before.z);
        CHECK(vel[1].x == 0.0f);
        CHECK(vel[1].y == 0.0f);
        CHECK(vel[1].z == 0.0f);
        // a live node still moved — forces are otherwise active.
        CHECK((pos[0].x != -2.0f || pos[0].y != 0.0f || pos[0].z != 0.0f));
    }
}

TEST_CASE("integrate: a disabled node exerts no repulsion on its neighbours") {
    auto run = [](bool disable_b, bool bh) {
        std::vector<Vector3> pos = {{-2.0f, 0, 0}, {-1.8f, 0, 0}};  // A, B close
        std::vector<Vector3> vel(2, Vector3{0, 0, 0});
        SimParams p{};
        p.use_barnes_hut = bh;
        const std::vector<char> dis =
            disable_b ? std::vector<char>{0, 1} : std::vector<char>{};
        integrate_step(pos, vel, {}, p, {}, dis);
        return pos[0].x;  // where node A landed
    };
    // Live B shoves A further -x via the strong close-range repulsion; disabled
    // B leaves A feeling only the centering pull toward 0 (+x). So the live-B
    // run ends with A more negative than the disabled-B run — on both paths.
    CHECK(run(false, false) < run(true, false));  // naive O(N^2)
    CHECK(run(false, true)  < run(true, true));   // barnes-hut
}

TEST_CASE("integrate: an edge to a disabled node does not pull") {
    SimParams p{};                // only the spring acts
    p.repulsion_k         = 0.0f;
    p.center_k            = 0.0f;
    p.phantom_repulsion_k = 0.0f;
    const std::vector<Edge> spring = {{0, 1}};  // stretched (rest 4, length 10)

    std::vector<Vector3> live = {{0, 0, 0}, {10, 0, 0}};
    std::vector<Vector3> vlive(2, Vector3{0, 0, 0});
    integrate_step(live, vlive, spring, p, {}, {});
    CHECK(live[0].x > 0.0f);  // A pulled toward B

    std::vector<Vector3> dead = {{0, 0, 0}, {10, 0, 0}};
    std::vector<Vector3> vdead(2, Vector3{0, 0, 0});
    integrate_step(dead, vdead, spring, p, {}, {0, 1});  // B disabled
    CHECK(dead[0].x == doctest::Approx(0.0f));  // edge skipped → no pull
    CHECK(dead[0].y == doctest::Approx(0.0f));
}
