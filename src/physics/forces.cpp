#include "physics/forces.h"

#include <cmath>

namespace zg::physics {

namespace {
constexpr float kMinDistance = 0.01f;
}

Vector3 coulomb_force(Vector3 a, Vector3 b, float charge_a, float charge_b, float k) {
    const float dx = a.x - b.x;
    const float dy = a.y - b.y;
    const float dz = a.z - b.z;
    float r2 = dx*dx + dy*dy + dz*dz;
    if (r2 < kMinDistance * kMinDistance) r2 = kMinDistance * kMinDistance;
    const float r = std::sqrt(r2);
    const float magnitude = k * charge_a * charge_b / r2;
    return {dx / r * magnitude, dy / r * magnitude, dz / r * magnitude};
}

Vector3 hooke_force(Vector3 a, Vector3 b, float rest_length, float stiffness) {
    const float dx = b.x - a.x;
    const float dy = b.y - a.y;
    const float dz = b.z - a.z;
    const float r = std::sqrt(dx*dx + dy*dy + dz*dz);
    if (r < kMinDistance) return {0.0f, 0.0f, 0.0f};
    const float displacement = r - rest_length;
    const float magnitude = stiffness * displacement;
    return {dx / r * magnitude, dy / r * magnitude, dz / r * magnitude};
}

}  // namespace zg::physics
