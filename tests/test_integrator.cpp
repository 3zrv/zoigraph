#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "physics/physics_thread.h"
#include "graph/graph_buffer.h"

#include <chrono>
#include <cmath>
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
