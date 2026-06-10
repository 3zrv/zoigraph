#pragma once

#include <raylib.h>

#include "app/session.h"
#include "input/escape_wipe.h"

namespace zg::render {

// Uniform locations for the CRT post-process shader. Captured once at
// startup (GetShaderLocation is expensive) and passed to composite_scene
// each frame so we don't look them up per-frame.
struct CrtShaderLocs {
    int resolution = -1;
    int time       = -1;
};

// Draws the off-screen scene render-texture onto the back buffer, going
// through the CRT shader when post_process is true. Caller is responsible
// for BeginDrawing / EndDrawing and any ClearBackground.
//
// RenderTextures store their pixels flipped vertically, so this passes a
// negative source height — don't undo that.
void composite_scene(const RenderTexture2D& scene_rt,
                     const Shader& crt_shader,
                     const CrtShaderLocs& crt_locs,
                     bool post_process);

// Draws edge labels (e.label) at the projected midpoint of each labelled
// edge. Must run AFTER composite_scene so labels stay crisp (the CRT
// shader would smear them) and BEFORE rlImGuiBegin so panels can sit on
// top. Labels whose midpoint is behind the camera are skipped because
// GetWorldToScreen returns nonsense for those.
void draw_edge_labels(const zg::app::Session& session, const Camera3D& camera);

// Triple-Escape progress indicator. Big red "ESC n/3" in the top-right
// while the count is non-zero and below 3 (3 exits the app).
// Runs after rlImGuiEnd so it sits on top of everything, ImGui included.
void draw_esc_hud(zg::input::EscapeWipe& esc_exit, double esc_window);

}  // namespace zg::render
