#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "physics/forces.h"

#include <cmath>

using zg::physics::coulomb_force;
using zg::physics::hooke_force;

namespace {
constexpr float kEps = 1e-4f;

bool near(float a, float b) { return std::fabs(a - b) < kEps; }
}

TEST_CASE("coulomb: equal charges push along the axis between them") {
    Vector3 a{0.0f, 0.0f, 0.0f};
    Vector3 b{2.0f, 0.0f, 0.0f};
    Vector3 f = coulomb_force(a, b, 1.0f, 1.0f, 4.0f);  // k=4, q=1, r=2 -> |F| = 4/4 = 1
    CHECK(near(f.x, -1.0f));
    CHECK(near(f.y, 0.0f));
    CHECK(near(f.z, 0.0f));
}

TEST_CASE("coulomb: action equals negative reaction") {
    Vector3 a{1.0f, 2.0f, 3.0f};
    Vector3 b{4.0f, 6.0f, 8.0f};
    Vector3 fa = coulomb_force(a, b, 1.0f, 1.0f, 1.0f);
    Vector3 fb = coulomb_force(b, a, 1.0f, 1.0f, 1.0f);
    CHECK(near(fa.x, -fb.x));
    CHECK(near(fa.y, -fb.y));
    CHECK(near(fa.z, -fb.z));
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
    CHECK(near(f.x, 0.0f));
    CHECK(near(f.y, 0.0f));
    CHECK(near(f.z, 0.0f));
}

TEST_CASE("hooke: stretched spring pulls toward partner") {
    Vector3 a{0.0f, 0.0f, 0.0f};
    Vector3 b{10.0f, 0.0f, 0.0f};
    Vector3 f = hooke_force(a, b, 5.0f, 0.5f);  // stretched by 5, k=0.5 -> |F|=2.5 toward +x
    CHECK(near(f.x, 2.5f));
    CHECK(near(f.y, 0.0f));
    CHECK(near(f.z, 0.0f));
}

TEST_CASE("hooke: compressed spring pushes away from partner") {
    Vector3 a{0.0f, 0.0f, 0.0f};
    Vector3 b{2.0f, 0.0f, 0.0f};
    Vector3 f = hooke_force(a, b, 5.0f, 0.5f);  // compressed by 3, force on a points -x
    CHECK(f.x < 0.0f);
}
