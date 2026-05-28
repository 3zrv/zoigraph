#include "render/labels.h"

#include <raymath.h>

namespace zg::render {

namespace {

bool alive(std::size_t i,
           const std::vector<zg::persistence::StoredNode>& nodes) {
    return i < nodes.size() && !nodes[i].deleted;
}

}  // namespace

std::set<std::size_t> compute_label_set(
    int selected,
    const std::vector<zg::graph::Edge>& edges,
    const std::vector<zg::telemetry::Phantom>& phantoms,
    const std::vector<std::size_t>& bones_chosen,
    const std::vector<zg::persistence::StoredNode>& stored_nodes,
    bool always_show) {

    std::set<std::size_t> out;

    if (always_show) {
        for (std::size_t i = 0; i < stored_nodes.size(); ++i) {
            if (!stored_nodes[i].deleted) out.insert(i);
        }
        return out;
    }

    if (selected >= 0) {
        const auto sel = static_cast<std::size_t>(selected);
        if (alive(sel, stored_nodes)) {
            out.insert(sel);
            // 1-hop neighbours, either direction
            for (const auto& e : edges) {
                if (e.source == sel && alive(e.target, stored_nodes)) {
                    out.insert(e.target);
                }
                if (e.target == sel && alive(e.source, stored_nodes)) {
                    out.insert(e.source);
                }
            }
        }
    }

    for (const auto& ph : phantoms) {
        for (const auto& c : ph.connections) {
            if (c.target < 0) continue;
            const auto t = static_cast<std::size_t>(c.target);
            if (alive(t, stored_nodes)) out.insert(t);
        }
    }

    for (std::size_t b : bones_chosen) {
        if (alive(b, stored_nodes)) out.insert(b);
    }

    return out;
}

void draw_node_labels(
    const std::set<std::size_t>& indices,
    const std::vector<Vector3>& positions,
    const std::vector<zg::persistence::StoredNode>& stored_nodes,
    const Camera3D& camera) {

    const Vector3 cam_forward = Vector3Subtract(camera.target, camera.position);
    for (std::size_t i : indices) {
        if (i >= positions.size() || i >= stored_nodes.size()) continue;
        if (stored_nodes[i].deleted) continue;
        const auto& title = stored_nodes[i].title;
        if (title.empty()) continue;
        const Vector3 p     = positions[i];
        const Vector3 to_p  = Vector3Subtract(p, camera.position);
        if (Vector3DotProduct(to_p, cam_forward) <= 0.0f) continue;
        const Vector2 screen = GetWorldToScreen(p, camera);
        const int     tw     = MeasureText(title.c_str(), 12);
        // 14 px above the projected centre so the text floats clear of
        // the wireframe halos. LIGHTGRAY reads as ambient rather than
        // competing with the maroon edge labels.
        DrawText(title.c_str(),
                 static_cast<int>(screen.x) - tw / 2,
                 static_cast<int>(screen.y) - 14,
                 12, LIGHTGRAY);
    }
}

}  // namespace zg::render
