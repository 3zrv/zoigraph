#pragma once

#include <raylib.h>

namespace zg::render {

inline constexpr Vector3 kCameraDefaultPos    = {60.0f, 60.0f, 60.0f};
inline constexpr Vector3 kCameraDefaultTarget = {0.0f, 0.0f, 0.0f};

// Custom orbit camera: right-drag rotates around the target, shift +
// right-drag pans, scroll dollies, R resets to the default pose. Gated
// against ImGui mouse-capture so dragging on a panel doesn't reach
// through to the 3D layer.
void update_orbit_camera(Camera3D& camera);

}  // namespace zg::render
