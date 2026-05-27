#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "physics/barnes_hut.h"
#include "physics/forces.h"

#include <cmath>
#include <random>
#include <vector>

using zg::physics::apply_barnes_hut_repulsion;
using zg::physics::coulomb_force;

namespace {

// Reference O(N^2) pairwise force sum, matching what integrate_step does
// when use_barnes_hut is false. Each particle has unit charge; force on i is
// the sum of pairwise Coulomb repulsions from every other particle.
std::vector<Vector3> naive_pairwise(const std::vector<Vector3>& positions, float k) {
    const std::size_t n = positions.size();
    std::vector<Vector3> forces(n, Vector3{0.0f, 0.0f, 0.0f});
    for (std::size_t i = 0; i < n; ++i) {
        for (std::size_t j = i + 1; j < n; ++j) {
            const Vector3 f = coulomb_force(positions[i], positions[j], 1.0f, 1.0f, k);
            forces[i].x += f.x; forces[i].y += f.y; forces[i].z += f.z;
            forces[j].x -= f.x; forces[j].y -= f.y; forces[j].z -= f.z;
        }
    }
    return forces;
}

float relative_error(Vector3 a, Vector3 b) {
    const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    const float diff = std::sqrt(dx*dx + dy*dy + dz*dz);
    const float mag  = std::sqrt(b.x*b.x + b.y*b.y + b.z*b.z);
    return mag > 1e-6f ? diff / mag : diff;
}

}  // namespace

TEST_CASE("barnes_hut: empty positions does not crash") {
    std::vector<Vector3> positions;
    std::vector<Vector3> forces;
    apply_barnes_hut_repulsion(positions, forces, 100.0f, 0.5f);
    CHECK(forces.empty());
}

TEST_CASE("barnes_hut: single particle has no force") {
    std::vector<Vector3> positions = {{1, 2, 3}};
    std::vector<Vector3> forces(1, Vector3{0, 0, 0});
    apply_barnes_hut_repulsion(positions, forces, 100.0f, 0.5f);
    CHECK(forces[0].x == doctest::Approx(0.0f));
    CHECK(forces[0].y == doctest::Approx(0.0f));
    CHECK(forces[0].z == doctest::Approx(0.0f));
}

TEST_CASE("barnes_hut: two particles repel each other symmetrically along the axis") {
    std::vector<Vector3> positions = {{0, 0, 0}, {2, 0, 0}};
    std::vector<Vector3> forces(2, Vector3{0, 0, 0});
    // |F| = k * 1 * 1 / r^2 = 4 / 4 = 1 along x.
    apply_barnes_hut_repulsion(positions, forces, 4.0f, 0.5f);

    CHECK(forces[0].x == doctest::Approx(-1.0f).epsilon(0.01f));
    CHECK(forces[0].y == doctest::Approx(0.0f));
    CHECK(forces[0].z == doctest::Approx(0.0f));
    CHECK(forces[1].x == doctest::Approx(1.0f).epsilon(0.01f));
}

TEST_CASE("barnes_hut: theta = 0 matches naive pairwise to high precision") {
    // theta = 0 disables the opening criterion, so the algorithm always
    // recurses to individual leaf particles. Result must be numerically
    // identical to the naive O(N^2) sum (modulo floating-point order of ops).
    std::vector<Vector3> positions = {
        {1.0f, 0.0f, 0.0f}, {-2.0f, 1.0f, 0.5f}, {0.0f, 3.0f, -1.5f},
        {4.5f, -2.0f, 1.0f}, {-1.0f, -1.0f, 2.5f}, {3.0f, 3.0f, 3.0f},
        {-3.5f, 0.5f, -2.0f}, {0.5f, -4.0f, 0.0f},
    };
    const float k = 50.0f;

    const auto naive = naive_pairwise(positions, k);

    std::vector<Vector3> bh(positions.size(), Vector3{0, 0, 0});
    apply_barnes_hut_repulsion(positions, bh, k, 0.0f);

    for (std::size_t i = 0; i < positions.size(); ++i) {
        CHECK(bh[i].x == doctest::Approx(naive[i].x).epsilon(1e-4));
        CHECK(bh[i].y == doctest::Approx(naive[i].y).epsilon(1e-4));
        CHECK(bh[i].z == doctest::Approx(naive[i].z).epsilon(1e-4));
    }
}

TEST_CASE("barnes_hut: theta=0.7 stays within ~10% of naive on a random cluster") {
    // With the conventional theta, internal cells get approximated; the
    // result is no longer exact but should track naive closely.
    std::mt19937 rng(0xBEEF);
    std::uniform_real_distribution<float> d(-15.0f, 15.0f);
    std::vector<Vector3> positions;
    for (int i = 0; i < 50; ++i) positions.push_back({d(rng), d(rng), d(rng)});

    const float k = 50.0f;
    const auto naive = naive_pairwise(positions, k);

    std::vector<Vector3> bh(positions.size(), Vector3{0, 0, 0});
    apply_barnes_hut_repulsion(positions, bh, k, 0.7f);

    int within = 0;
    for (std::size_t i = 0; i < positions.size(); ++i) {
        if (relative_error(bh[i], naive[i]) < 0.10f) ++within;
    }
    // Nearly every particle should be within 10%; allow a couple of stragglers
    // at the cluster edges where the approximation has less to work with.
    CHECK(within >= static_cast<int>(positions.size()) - 3);
}

TEST_CASE("barnes_hut: coincident particles don't produce NaN or inf") {
    std::vector<Vector3> positions = {
        {1.0f, 1.0f, 1.0f}, {1.0f, 1.0f, 1.0f},
        {1.0f, 1.0f, 1.0f}, {1.0f, 1.0f, 1.0f},
    };
    std::vector<Vector3> forces(positions.size(), Vector3{0, 0, 0});
    apply_barnes_hut_repulsion(positions, forces, 100.0f, 0.5f);
    for (const auto& f : forces) {
        CHECK_FALSE(std::isnan(f.x));
        CHECK_FALSE(std::isnan(f.y));
        CHECK_FALSE(std::isnan(f.z));
        CHECK_FALSE(std::isinf(f.x));
        CHECK_FALSE(std::isinf(f.y));
        CHECK_FALSE(std::isinf(f.z));
    }
}

TEST_CASE("barnes_hut: 200-particle stress stays close to naive at theta=0.7") {
    // Scaled-up version of the smaller correctness test: a real-ish cluster
    // size, default theta, expect most particles within 15% of naive.
    std::mt19937 rng(0xCAFEBABE);
    std::uniform_real_distribution<float> d(-20.0f, 20.0f);
    std::vector<Vector3> positions;
    positions.reserve(200);
    for (int i = 0; i < 200; ++i) positions.push_back({d(rng), d(rng), d(rng)});

    const float k = 60.0f;
    const auto naive = naive_pairwise(positions, k);

    std::vector<Vector3> bh(positions.size(), Vector3{0, 0, 0});
    apply_barnes_hut_repulsion(positions, bh, k, 0.7f);

    int within = 0;
    for (std::size_t i = 0; i < positions.size(); ++i) {
        if (relative_error(bh[i], naive[i]) < 0.15f) ++within;
    }
    CHECK(within >= static_cast<int>(positions.size()) * 9 / 10);
}

TEST_CASE("barnes_hut: theta=0.3 (stricter) is even closer to naive than 0.7") {
    // Lower theta -> more recursion into subdivisions -> closer to naive.
    std::mt19937 rng(0xDEAD);
    std::uniform_real_distribution<float> d(-15.0f, 15.0f);
    std::vector<Vector3> positions;
    for (int i = 0; i < 30; ++i) positions.push_back({d(rng), d(rng), d(rng)});

    const float k = 50.0f;
    const auto naive = naive_pairwise(positions, k);

    std::vector<Vector3> bh_default(30, Vector3{0, 0, 0});
    std::vector<Vector3> bh_strict (30, Vector3{0, 0, 0});
    apply_barnes_hut_repulsion(positions, bh_default, k, 0.7f);
    apply_barnes_hut_repulsion(positions, bh_strict,  k, 0.3f);

    float total_default_err = 0.0f, total_strict_err = 0.0f;
    for (std::size_t i = 0; i < positions.size(); ++i) {
        total_default_err += relative_error(bh_default[i], naive[i]);
        total_strict_err  += relative_error(bh_strict[i],  naive[i]);
    }
    CHECK(total_strict_err <= total_default_err + 1e-4f);
}

TEST_CASE("barnes_hut: huge repulsion_k near-coincident produces no NaN/inf") {
    std::vector<Vector3> positions = {{0.001f, 0, 0}, {-0.001f, 0, 0}};
    std::vector<Vector3> forces(2, Vector3{0, 0, 0});
    apply_barnes_hut_repulsion(positions, forces, 1.0e8f, 0.5f);
    for (const auto& f : forces) {
        CHECK_FALSE(std::isnan(f.x));
        CHECK_FALSE(std::isinf(f.x));
    }
}

TEST_CASE("barnes_hut: forces are added to, not overwritten") {
    // The caller may already have accumulated Hooke / centering / phantom
    // forces by the time the BH pass runs. Verify those contributions
    // survive an apply call.
    std::vector<Vector3> positions = {{0, 0, 0}, {2, 0, 0}};
    std::vector<Vector3> forces    = {{5, 5, 5}, {-3, 0, 0}};

    apply_barnes_hut_repulsion(positions, forces, 4.0f, 0.5f);

    // BH adds about -1 in x to forces[0] and +1 in x to forces[1].
    CHECK(forces[0].x == doctest::Approx(4.0f).epsilon(0.01f));
    CHECK(forces[0].y == doctest::Approx(5.0f));
    CHECK(forces[0].z == doctest::Approx(5.0f));
    CHECK(forces[1].x == doctest::Approx(-2.0f).epsilon(0.01f));
}
