#include "app/hotkeys.h"

#include <imgui.h>
#include <raylib.h>
#include <raymath.h>

#include <nlohmann/json.hpp>

#include <cstddef>
#include <utility>
#include <vector>

#include "graph/timeline.h"
#include "persistence/db.h"
#include "render/camera.h"

namespace zg::app {

void handle_hotkeys(Session& s,
                    Camera3D& camera,
                    zg::input::EscapeWipe& esc_wipe,
                    double wipe_window,
                    zg::macros::RabbitHole& rabbit,
                    zg::macros::Bones& bones,
                    std::mt19937& rng,
                    bool& requested_exit) {
    auto& physics       = s.physics;
    auto& stored_nodes  = s.stored_nodes;
    auto& edges         = s.edges;
    auto& positions     = s.positions;
    auto& self_idx      = s.self_idx;
    auto& selected_node = s.selected_node;
    auto& timeline_mode = s.timeline_mode;

    if (IsKeyPressed(KEY_ESCAPE) && esc_wipe.record(GetTime(), wipe_window)) {
        requested_exit = true;
    }

    const bool typing = ImGui::GetIO().WantTextInput;

    if (IsKeyPressed(KEY_H) && !typing && selected_node >= 0
        && static_cast<std::size_t>(selected_node) < positions.size()
        && !rabbit.active && !bones.active) {
        auto path = zg::macros::pick_rabbit_path(
            static_cast<std::size_t>(selected_node),
            edges, rng, positions.size());
        if (path.size() >= 2) {
            rabbit.active        = true;
            rabbit.path          = std::move(path);
            rabbit.segment       = 0;
            rabbit.elapsed       = 0.0f;
            rabbit.camera_offset = Vector3Subtract(camera.position, camera.target);
        }
    }

    if (IsKeyPressed(KEY_B) && !typing && !rabbit.active && !bones.active
        && positions.size() >= 3) {
        zg::macros::throw_bones(bones, positions, edges, camera, rng);
        // Only log on a successful throw -- pick_weakly_connected_triple
        // returns <3 ids when the graph can't supply a meaningful triple,
        // in which case bones.active stays false and the macro is a no-op.
        if (bones.active && bones.chosen.size() == 3 && s.db) {
            nlohmann::json p = {
                {"chosen", bones.chosen},
            };
            s.db->log_event("bones_throw", -1, p.dump());
        }
    }

    if (IsKeyPressed(KEY_T) && !typing && physics && !stored_nodes.empty()) {
        timeline_mode = !timeline_mode;
        if (timeline_mode) {
            std::vector<double> firsts;
            firsts.reserve(stored_nodes.size());
            for (const auto& sn : stored_nodes) firsts.push_back(sn.first_seen);
            const auto layout = zg::graph::compute_timeline_positions(firsts);
            for (std::size_t i = 0; i < layout.size(); ++i) {
                physics->set_pin(i, layout[i]);
            }
        } else {
            for (std::size_t i = 0; i < stored_nodes.size(); ++i) {
                if (i != self_idx) physics->clear_pin(i);
            }
        }
    }

    // Camera step: active macro drives the camera; otherwise free orbit.
    if (rabbit.active) {
        zg::macros::update_rabbit_hole(rabbit, camera, positions, selected_node, GetFrameTime());
    } else if (bones.active) {
        zg::macros::update_bones(bones, camera, GetFrameTime());
    } else {
        zg::render::update_orbit_camera(camera);
    }
}

}  // namespace zg::app
