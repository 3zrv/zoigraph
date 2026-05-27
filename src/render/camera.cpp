#include "render/camera.h"

#include <raymath.h>
#include <imgui.h>

#include <algorithm>
#include <cmath>

namespace zg::render {

void update_orbit_camera(Camera3D& camera) {
    const bool ui_has_mouse = ImGui::GetIO().WantCaptureMouse;
    const Vector2 dm        = GetMouseDelta();
    const float wheel       = GetMouseWheelMove();
    const bool shift_down   = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);

    if (!ui_has_mouse) {
        const bool dragging = IsMouseButtonDown(MOUSE_BUTTON_RIGHT) &&
                              (dm.x != 0.0f || dm.y != 0.0f);

        if (dragging && shift_down) {
            const Vector3 offset = Vector3Subtract(camera.position, camera.target);
            const Vector3 right  = Vector3Normalize(Vector3CrossProduct(camera.up, Vector3Negate(offset)));
            const Vector3 up     = Vector3Normalize(Vector3CrossProduct(Vector3Negate(offset), right));
            const float scale    = Vector3Length(offset) * 0.0015f;
            const Vector3 pan    = Vector3Add(Vector3Scale(right, -dm.x * scale),
                                              Vector3Scale(up,    dm.y * scale));
            camera.position = Vector3Add(camera.position, pan);
            camera.target   = Vector3Add(camera.target,   pan);
        } else if (dragging) {
            Vector3 offset = Vector3Subtract(camera.position, camera.target);

            // Yaw around the world up axis.
            offset = Vector3RotateByAxisAngle(offset, camera.up, -dm.x * 0.005f);

            // Pitch around the camera-right axis, clamped to avoid gimbal flip.
            const Vector3 right = Vector3Normalize(Vector3CrossProduct(camera.up, Vector3Negate(offset)));
            const Vector3 pitched = Vector3RotateByAxisAngle(offset, right, -dm.y * 0.005f);
            const Vector3 dir = Vector3Normalize(pitched);
            if (std::fabs(Vector3DotProduct(dir, camera.up)) < 0.985f) {
                offset = pitched;
            }
            camera.position = Vector3Add(camera.target, offset);
        }

        if (wheel != 0.0f) {
            Vector3 offset = Vector3Subtract(camera.position, camera.target);
            float distance = Vector3Length(offset);
            distance = std::clamp(distance * (1.0f - wheel * 0.1f), 2.0f, 500.0f);
            offset = Vector3Scale(Vector3Normalize(offset), distance);
            camera.position = Vector3Add(camera.target, offset);
        }
    }

    if (IsKeyPressed(KEY_R)) {
        camera.position = kCameraDefaultPos;
        camera.target   = kCameraDefaultTarget;
    }
}

}  // namespace zg::render
