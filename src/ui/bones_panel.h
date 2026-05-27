#pragma once

#include <raylib.h>

#include <random>

#include "app/session.h"
#include "macros/bones.h"

namespace zg::ui {

// Bones scratch panel — a separate ImGui window that opens after a throw
// picks three nodes and stays open until the operator closes it.
// Each chosen-node row is a Selectable: clicking flies the camera and
// updates session.selected_node. "throw again" re-rolls in place.
//
// Caller is expected to have called rlImGuiBegin() and to NOT have
// called rlImGuiEnd() yet. main_w / main_h come from the main panel so
// the bones window can sit side-by-side when there's room, or stack
// below when there isn't. Does nothing when bones.panel_open is false.
void render_bones_panel(zg::macros::Bones& bones,
                        zg::app::Session& session,
                        Camera3D& camera,
                        float main_w,
                        float main_h,
                        std::mt19937& rng);

}  // namespace zg::ui
