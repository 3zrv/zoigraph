#include "app/pick.h"

#include <imgui.h>
#include <raylib.h>

#include <cstddef>
#include <utility>

#include "app/clock.h"
#include "graph/types.h"
#include "persistence/db.h"
#include "render/sizes.h"

namespace zg::app {

void handle_pick(Session& s,
                 const Camera3D& camera,
                 const std::vector<zg::telemetry::Phantom>& phantoms,
                 zg::telemetry::PhantomBuffer& phantom_buffer,
                 DoubleClickState& dbl,
                 bool& focus_inspector) {
    if (!IsMouseButtonPressed(MOUSE_BUTTON_LEFT) || ImGui::GetIO().WantCaptureMouse) {
        return;
    }

    auto& db            = s.db;
    auto& physics       = s.physics;
    auto& stored_nodes  = s.stored_nodes;
    auto& edges         = s.edges;
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
        // graph edges, save to disk, queue both node and edges into
        // physics, then select. Promoted node enters the "phantom" tier
        // (visibly distinct from confirmed Static Nodes).
        const auto& ph = phantoms[phantom_hit];
        const long long new_id   = static_cast<long long>(stored_nodes.size());
        const double promoted_ts = unix_now();
        zg::persistence::StoredNode promoted{};
        promoted.id           = new_id;
        promoted.position     = ph.position;
        promoted.title        = ph.label;
        promoted.content      = "";
        promoted.first_seen   = promoted_ts;
        promoted.last_touched = promoted_ts;
        promoted.tier         = "phantom";
        stored_nodes.push_back(std::move(promoted));

        for (long long target_id : ph.connections) {
            if (target_id < 0) continue;
            const auto tidx = static_cast<std::size_t>(target_id);
            if (tidx >= positions.size()) continue;
            if (tidx == static_cast<std::size_t>(new_id)) continue;
            const zg::graph::Edge new_edge{
                static_cast<std::size_t>(new_id), tidx};
            edges.push_back(new_edge);
            physics->enqueue_edge(new_edge);
        }

        phantom_buffer.remove(ph.id);
        db->save_graph(stored_nodes, edges);
        physics->enqueue_node(ph.position);
        selected_node = static_cast<int>(new_id);
    } else {
        float best_dist = 0.0f;
        int   best_idx  = -1;
        for (std::size_t i = 0; i < positions.size(); ++i) {
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
