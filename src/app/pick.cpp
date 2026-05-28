#include "app/pick.h"

#include <imgui.h>
#include <raylib.h>

#include <nlohmann/json.hpp>

#include <cstddef>
#include <utility>

#include "app/clock.h"
#include "app/promote.h"
#include "graph/types.h"
#include "persistence/db.h"
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

        // promote_phantom is the pure-logic version of click-to-pin
        // (covered by test_promote). Edge dropping rules and the trust-
        // gradient defaults (node tier + edge certainty = "phantom")
        // live there; this site just consumes the result.
        auto promo = promote_phantom(ph, new_id, promoted_ts, positions.size());
        stored_nodes.push_back(std::move(promo.node));
        for (const auto& e : promo.edges) {
            edges.push_back(e);
            physics->enqueue_edge(e);
        }

        phantom_buffer.remove(ph.id);
        db->save_graph(stored_nodes, edges);
        physics->enqueue_node(ph.position);
        selected_node = static_cast<int>(new_id);

        // Log the pin and remove this id from the
        // spawn tracker so main's per-frame diff doesn't also fire a decay
        // event for the same phantom. time_to_pin_s is the wall-clock gap
        // between the spawn UDP packet landing and the operator clicking;
        // it's the headline behavioural signal for the trust-gradient
        // test.
        const auto spawn_it = seen_phantom_spawn.find(ph.id);
        const double time_to_pin = (spawn_it != seen_phantom_spawn.end())
            ? (GetTime() - spawn_it->second) : 0.0;
        if (spawn_it != seen_phantom_spawn.end()) {
            seen_phantom_spawn.erase(spawn_it);
        }
        if (db) {
            nlohmann::json conns = nlohmann::json::array();
            for (const auto& c : ph.connections) {
                conns.push_back({{"target", c.target}, {"kind", c.kind}});
            }
            nlohmann::json payload = {
                {"phantom_id",    ph.id},
                {"new_node_id",   new_id},
                {"label",         ph.label},
                {"content",       ph.content},
                {"connections",   conns},
                {"time_to_pin_s", time_to_pin},
                {"source",        ph.source},
            };
            db->log_event("phantom_pin", new_id, payload.dump());
        }
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
