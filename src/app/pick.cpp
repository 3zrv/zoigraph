#include "app/pick.h"

#include <imgui.h>
#include <raylib.h>

#include <cstddef>

#include "app/pin.h"
#include "render/sizes.h"

namespace zg::app {

void handle_pick(Session& s,
                 const Camera3D& camera,
                 const std::vector<zg::telemetry::Phantom>& phantoms,
                 zg::telemetry::PhantomBuffer& phantom_buffer,
                 DoubleClickState& dbl,
                 bool& focus_inspector,
                 std::unordered_map<long long, double>& seen_phantom_spawn) {
    if (!IsMouseButtonPressed(MOUSE_BUTTON_LEFT) || ImGui::GetIO().WantCaptureMouse) {
        return;
    }

    auto& stored_nodes  = s.stored_nodes;
    auto& positions     = s.positions;
    auto& selected_node = s.selected_node;

    const Ray ray = GetScreenToWorldRay(GetMousePosition(), camera);

    // Phantoms get first crack since they're bigger and ephemeral; hitting
    // one promotes it before the static-pick pass even runs.
    int    phantom_hit  = -1;
    float  phantom_dist = 0.0f;
    for (std::size_t i = 0; i < phantoms.size(); ++i) {
        const RayCollision hit = GetRayCollisionSphere(
            ray, phantoms[i].position, zg::render::kPhantomRadius);
        if (hit.hit && (phantom_hit < 0 || hit.distance < phantom_dist)) {
            phantom_dist = hit.distance;
            phantom_hit  = static_cast<int>(i);
        }
    }

    if (phantom_hit >= 0) {
        // Promote: halt decay, append a Static Node carrying the phantom's
        // label as title, materialize any jagged-edge connections as real
        // graph edges, persist incrementally, queue into physics, select,
        // log the phantom_pin event. All of it lives in pin_phantom
        // (app/pin.cpp) — shared with the CLI /pin command so the two
        // accept paths can't drift on side-effects or event logging.
        pin_phantom(s, phantoms[phantom_hit], phantom_buffer,
                    seen_phantom_spawn);
    } else {
        float best_dist = 0.0f;
        int   best_idx  = -1;
        for (std::size_t i = 0; i < positions.size(); ++i) {
            // Tombstoned nodes aren't rendered and can't be picked.
            if (i < stored_nodes.size() && stored_nodes[i].deleted) continue;
            const RayCollision hit = GetRayCollisionSphere(
                ray, positions[i], zg::render::kNodeRadius);
            if (hit.hit && (best_idx < 0 || hit.distance < best_dist)) {
                best_dist = hit.distance;
                best_idx  = static_cast<int>(i);
            }
        }
        selected_node = best_idx;
    }

    // Double-click on the same node within 350 ms surfaces the Inspector
    // tab next frame. Consume the tracker on detection so a fast triple-
    // click doesn't fire twice.
    if (selected_node >= 0) {
        const double t = GetTime();
        if (selected_node == dbl.last_idx && (t - dbl.last_t) < 0.35) {
            focus_inspector = true;
            dbl.last_idx    = -1;
        } else {
            dbl.last_idx = selected_node;
            dbl.last_t   = t;
        }
    }
}

}  // namespace zg::app
