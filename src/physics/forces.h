#pragma once

#include <raylib.h>

namespace zg::physics {

// Coulomb repulsion: F on `a` from `b`, pointing away from b.
// |F| = k * q_a * q_b / r^2, capped near zero to avoid singularity.
Vector3 coulomb_force(Vector3 a, Vector3 b, float charge_a, float charge_b, float k);

// Hooke spring: F on `a` pulling toward `b`.
// |F| = stiffness * (current_length - rest_length), along (b - a).
Vector3 hooke_force(Vector3 a, Vector3 b, float rest_length, float stiffness);

}  // namespace zg::physics
