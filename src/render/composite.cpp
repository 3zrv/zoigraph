#include "render/composite.h"

#include <raylib.h>
#include <raymath.h>

#include <cstddef>

namespace zg::render {

void composite_scene(const RenderTexture2D& scene_rt,
                     const Shader& crt_shader,
                     const CrtShaderLocs& crt_locs,
                     bool post_process) {
    const float scr_w = static_cast<float>(GetScreenWidth());
    const float scr_h = static_cast<float>(GetScreenHeight());
    if (post_process) {
        const Vector2 res{scr_w, scr_h};
        const float   t = static_cast<float>(GetTime());
        SetShaderValue(crt_shader, crt_locs.resolution, &res, SHADER_UNIFORM_VEC2);
        SetShaderValue(crt_shader, crt_locs.time,       &t,   SHADER_UNIFORM_FLOAT);
        BeginShaderMode(crt_shader);
    }
    const Rectangle src{0, 0,
                        static_cast<float>(scene_rt.texture.width),
                        -static_cast<float>(scene_rt.texture.height)};
    const Rectangle dst{0, 0, scr_w, scr_h};
    DrawTexturePro(scene_rt.texture, src, dst, {0, 0}, 0.0f, WHITE);
    if (post_process) {
        EndShaderMode();
    }
}

void draw_edge_labels(const zg::app::Session& s, const Camera3D& camera) {
    const auto& edges     = s.edges;
    const auto& positions = s.positions;
    const Vector3 cam_forward = Vector3Subtract(camera.target, camera.position);
    for (const auto& e : edges) {
        if (e.label.empty()) continue;
        if (e.source >= positions.size() || e.target >= positions.size()) continue;
        const Vector3 mid    = Vector3Lerp(positions[e.source], positions[e.target], 0.5f);
        const Vector3 to_mid = Vector3Subtract(mid, camera.position);
        if (Vector3DotProduct(to_mid, cam_forward) <= 0.0f) continue;
        const Vector2 screen = GetWorldToScreen(mid, camera);
        const int tw = MeasureText(e.label.c_str(), 12);
        DrawText(e.label.c_str(),
                 static_cast<int>(screen.x) - tw / 2,
                 static_cast<int>(screen.y) - 7,
                 12, GRAY);
    }
}

void draw_esc_hud(zg::input::EscapeWipe& esc_wipe, double wipe_window) {
    const int esc_recent = esc_wipe.count_recent(GetTime(), wipe_window);
    if (esc_recent > 0 && esc_recent < 3) {
        const char* msg = TextFormat("ESC %d/3", esc_recent);
        const int   tw  = MeasureText(msg, 36);
        DrawText(msg, GetScreenWidth() - tw - 24, 24, 36, RED);
    }
}

}  // namespace zg::render
