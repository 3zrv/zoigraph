#pragma once

#include <raylib.h>

#include <random>

#include "app/session.h"
#include "input/escape_wipe.h"
#include "macros/bones.h"
#include "macros/rabbit_hole.h"

namespace zg::app {

// Per-frame keyboard handling + camera step for the render loop.
// Each key is gated on !ImGui::GetIO().WantTextInput so the operator
// can type into the inspector without firing macros.
//
//   ESC          recorded against esc_wipe; three within wipe_window
//                seconds sets requested_exit
//   H            from selected_node, picks a 3-segment rabbit-hole path
//                and starts the macro (ignored if a macro is running)
//   B            throws the bones (3 weakly-connected nodes, smooth fly)
//   T            toggles timeline mode (pins every node on a time axis,
//                or releases them so physics takes over again)
//
// After key handling, steps the camera one frame: rabbit and bones
// macros each drive the camera while active; otherwise orbit/pan/zoom
// is delegated to update_orbit_camera.
//
// Mutates session.{timeline_mode, selected_node, positions via physics
// pin/unpin}, the rabbit/bones structs, esc_wipe, requested_exit, and
// camera.
void handle_hotkeys(Session& session,
                    Camera3D& camera,
                    zg::input::EscapeWipe& esc_wipe,
                    double wipe_window,
                    zg::macros::RabbitHole& rabbit,
                    zg::macros::Bones& bones,
                    std::mt19937& rng,
                    bool& requested_exit);

}  // namespace zg::app
