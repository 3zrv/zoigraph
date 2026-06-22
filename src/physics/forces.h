#pragma once

#include <raylib.h>

#include <vector>

namespace zg::physics {

// Coulomb repulsion: F on `a` from `b`, pointing away from b.
// |F| = k * q_a * q_b / r^2, capped near zero to avoid singularity.
Vector3 coulomb_force(Vector3 a, Vector3 b, float charge_a, float charge_b, float k);

// Hooke spring: F on `a` pulling toward `b`.
// |F| = stiffness * (current_length - rest_length), along (b - a).
Vector3 hooke_force(Vector3 a, Vector3 b, float rest_length, float stiffness);

// Mean squared speed across all velocities (0.0 for an empty set). Drives the
// physics thread's freeze-on-convergence: when the *whole graph's* RMS speed
// stays below a threshold the layout has essentially stopped rearranging, so
// stepping can pause until something perturbs it. Mean (not max) so a handful
// of slow-to-settle straggler nodes don't keep the entire sim awake forever.
// Squared to avoid a per-node sqrt — callers compare against eps*eps.
float mean_speed_squared(const std::vector<Vector3>& velocities);

}  // namespace zg::physics
