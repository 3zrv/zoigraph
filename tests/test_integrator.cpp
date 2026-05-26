#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "physics/physics_thread.h"

#include <cmath>
#include <vector>

using zg::physics::SimParams;
using zg::physics::integrate_step;
using zg::graph::Edge;

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
