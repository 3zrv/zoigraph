#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "physics/forces.h"

#include <cmath>

using zg::physics::coulomb_force;
using zg::physics::hooke_force;

namespace {
constexpr float kEps = 1e-4f;

bool near_eq(float a, float b) { return std::fabs(a - b) < kEps; }
}

TEST_CASE("coulomb: equal charges push along the axis between them") {
    Vector3 a{0.0f, 0.0f, 0.0f};
    Vector3 b{2.0f, 0.0f, 0.0f};
    Vector3 f = coulomb_force(a, b, 1.0f, 1.0f, 4.0f);  // k=4, q=1, r=2 -> |F| = 4/4 = 1
    CHECK(near_eq(f.x, -1.0f));
    CHECK(near_eq(f.y, 0.0f));
    CHECK(near_eq(f.z, 0.0f));
}

TEST_CASE("coulomb: action equals negative reaction") {
    Vector3 a{1.0f, 2.0f, 3.0f};
    Vector3 b{4.0f, 6.0f, 8.0f};
    Vector3 fa = coulomb_force(a, b, 1.0f, 1.0f, 1.0f);
    Vector3 fb = coulomb_force(b, a, 1.0f, 1.0f, 1.0f);
    CHECK(near_eq(fa.x, -fb.x));
    CHECK(near_eq(fa.y, -fb.y));
    CHECK(near_eq(fa.z, -fb.z));
}

TEST_CASE("coulomb: opposite charges attract") {
    Vector3 a{0.0f, 0.0f, 0.0f};
    Vector3 b{2.0f, 0.0f, 0.0f};
    Vector3 f = coulomb_force(a, b, 1.0f, -1.0f, 4.0f);  // negative product -> force points TOWARD b
    CHECK(f.x > 0.0f);
}

TEST_CASE("coulomb: near-zero distance is clamped, no NaN") {
    Vector3 a{0.0f, 0.0f, 0.0f};
    Vector3 b{0.0f, 0.0f, 0.0f};
    Vector3 f = coulomb_force(a, b, 1.0f, 1.0f, 1.0f);
    CHECK_FALSE(std::isnan(f.x));
    CHECK_FALSE(std::isnan(f.y));
    CHECK_FALSE(std::isnan(f.z));
}

TEST_CASE("hooke: at rest length the force is zero") {
    Vector3 a{0.0f, 0.0f, 0.0f};
    Vector3 b{5.0f, 0.0f, 0.0f};
    Vector3 f = hooke_force(a, b, 5.0f, 0.5f);
    CHECK(near_eq(f.x, 0.0f));
    CHECK(near_eq(f.y, 0.0f));
    CHECK(near_eq(f.z, 0.0f));
}

TEST_CASE("hooke: stretched spring pulls toward partner") {
    Vector3 a{0.0f, 0.0f, 0.0f};
    Vector3 b{10.0f, 0.0f, 0.0f};
    Vector3 f = hooke_force(a, b, 5.0f, 0.5f);  // stretched by 5, k=0.5 -> |F|=2.5 toward +x
    CHECK(near_eq(f.x, 2.5f));
    CHECK(near_eq(f.y, 0.0f));
    CHECK(near_eq(f.z, 0.0f));
}

TEST_CASE("hooke: compressed spring pushes away from partner") {
    Vector3 a{0.0f, 0.0f, 0.0f};
    Vector3 b{2.0f, 0.0f, 0.0f};
    Vector3 f = hooke_force(a, b, 5.0f, 0.5f);  // compressed by 3, force on a points -x
    CHECK(f.x < 0.0f);
}

TEST_CASE("coulomb: k = 0 yields zero force regardless of geometry") {
    Vector3 f = coulomb_force({1, 2, 3}, {4, 5, 6}, 1.0f, 1.0f, 0.0f);
    CHECK(near_eq(f.x, 0.0f));
    CHECK(near_eq(f.y, 0.0f));
    CHECK(near_eq(f.z, 0.0f));
}

TEST_CASE("coulomb: 3D diagonal direction is correctly normalized") {
    // Unit charges, k=1, separated by (1,1,1). r^2=3, magnitude = 1/3.
    // Direction (1,1,1)/sqrt(3). Each component = magnitude * 1/sqrt(3).
    Vector3 f = coulomb_force({0, 0, 0}, {1, 1, 1}, 1.0f, 1.0f, 1.0f);
    const float expected = -1.0f / (3.0f * std::sqrt(3.0f));
    CHECK(near_eq(f.x, expected));
    CHECK(near_eq(f.y, expected));
    CHECK(near_eq(f.z, expected));
}

TEST_CASE("hooke: identical positions yield zero force and no NaN") {
    Vector3 f = hooke_force({3, 4, 5}, {3, 4, 5}, 1.0f, 0.5f);
    CHECK(near_eq(f.x, 0.0f));
    CHECK(near_eq(f.y, 0.0f));
    CHECK(near_eq(f.z, 0.0f));
    CHECK_FALSE(std::isnan(f.x));
    CHECK_FALSE(std::isnan(f.y));
    CHECK_FALSE(std::isnan(f.z));
}

TEST_CASE("hooke: stretched 3D diagonal pulls along the correct axis") {
    // Distance = sqrt(12), rest=1, stretch = sqrt(12) - 1.
    // F on a toward b along (2,2,2)/sqrt(12).
    Vector3 f = hooke_force({0, 0, 0}, {2, 2, 2}, 1.0f, 1.0f);
    const float r        = std::sqrt(12.0f);
    const float expected = (r - 1.0f) * 2.0f / r;
    CHECK(near_eq(f.x, expected));
    CHECK(near_eq(f.y, expected));
    CHECK(near_eq(f.z, expected));
}

TEST_CASE("hooke: zero stiffness yields zero force") {
    Vector3 f = hooke_force({0, 0, 0}, {10, 0, 0}, 1.0f, 0.0f);
    CHECK(near_eq(f.x, 0.0f));
    CHECK(near_eq(f.y, 0.0f));
    CHECK(near_eq(f.z, 0.0f));
}

TEST_CASE("hooke: action equals negative reaction for any pair") {
    // Newton's third law for the spring: swapping a and b flips the sign
    // exactly. Sample several geometries to catch axis-specific bugs.
    const Vector3 cases[][2] = {
        {{1, 0, 0}, {5, 0, 0}},
        {{0, 0, 0}, {0, 3, 4}},
        {{-1, -2, -3}, {4, 5, 6}},
    };
    for (const auto& c : cases) {
        const Vector3 fa = hooke_force(c[0], c[1], 1.0f, 0.5f);
        const Vector3 fb = hooke_force(c[1], c[0], 1.0f, 0.5f);
        CHECK(near_eq(fa.x, -fb.x));
        CHECK(near_eq(fa.y, -fb.y));
        CHECK(near_eq(fa.z, -fb.z));
    }
}

TEST_CASE("coulomb: force magnitude falls off as 1 over r squared") {
    // |F| = k * qa * qb / r^2. Doubling r should quarter the force.
    auto magnitude = [](Vector3 v) {
        return std::sqrt(v.x*v.x + v.y*v.y + v.z*v.z);
    };
    const Vector3 f_near = coulomb_force({0, 0, 0}, {2, 0, 0}, 1.0f, 1.0f, 1.0f);
    const Vector3 f_far  = coulomb_force({0, 0, 0}, {4, 0, 0}, 1.0f, 1.0f, 1.0f);

    // m_near = 1/4, m_far = 1/16 → ratio 4.
    CHECK(magnitude(f_near) / magnitude(f_far) == doctest::Approx(4.0f).epsilon(0.01f));
}

TEST_CASE("coulomb: zero charge on either endpoint yields zero force") {
    // Force magnitude is k * q_a * q_b / r^2 — if either charge is zero
    // the whole product is zero regardless of how close the two are.
    Vector3 f1 = coulomb_force({0, 0, 0}, {1, 0, 0}, 0.0f, 1.0f, 100.0f);
    Vector3 f2 = coulomb_force({0, 0, 0}, {1, 0, 0}, 1.0f, 0.0f, 100.0f);
    for (const auto& f : {f1, f2}) {
        CHECK(near_eq(f.x, 0.0f));
        CHECK(near_eq(f.y, 0.0f));
        CHECK(near_eq(f.z, 0.0f));
    }
}

TEST_CASE("hooke: rest_length much larger than current distance pushes apart") {
    // Spring is heavily compressed (rest=20, current=1) → force on a should
    // point away from b (negative x for a at origin, b at +x).
    Vector3 f = hooke_force({0, 0, 0}, {1, 0, 0}, 20.0f, 1.0f);
    CHECK(f.x < 0.0f);  // a is pushed away from b
    // Magnitude = stiffness * |stretch - rest| = 1 * 19 = 19, direction -x.
    CHECK(f.x == doctest::Approx(-19.0f).epsilon(0.01f));
}

TEST_CASE("hooke: rest_length of zero gives pure attraction along the axis") {
    // With rest = 0, the spring tries to collapse the two points. Force on
    // `a` should point toward `b` with magnitude stiffness * distance.
    Vector3 f = hooke_force({0, 0, 0}, {3, 4, 0}, 0.0f, 1.0f);
    // distance = 5, magnitude = 5, direction = (3/5, 4/5, 0).
    CHECK(near_eq(f.x, 3.0f));
    CHECK(near_eq(f.y, 4.0f));
    CHECK(near_eq(f.z, 0.0f));
}

TEST_CASE("mean_speed_squared: empty set is zero") {
    CHECK(zg::physics::mean_speed_squared({}) == 0.0f);
}

TEST_CASE("mean_speed_squared: averages the squared speeds") {
    // speeds^2: (3,4,0)->25, (0,0,0)->0, (0,0,5)->25, (1,0,0)->1. mean = 51/4.
    std::vector<Vector3> v = {{3, 4, 0}, {0, 0, 0}, {0, 0, 5}, {1, 0, 0}};
    CHECK(zg::physics::mean_speed_squared(v) == doctest::Approx(51.0f / 4.0f));
}

TEST_CASE("mean_speed_squared: a few fast stragglers wash out across many at rest") {
    // 999 nodes at rest + 1 moving fast => mean stays tiny, unlike a max.
    std::vector<Vector3> v(999, Vector3{0, 0, 0});
    v.push_back({10, 0, 0});  // speed^2 = 100
    CHECK(zg::physics::mean_speed_squared(v) == doctest::Approx(100.0f / 1000.0f));
}
