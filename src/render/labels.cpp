#include "render/labels.h"

#include <raymath.h>

#include "render/sizes.h"

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

    constexpr int   kFontSize     = 16;
    constexpr float kSpacing      = 2.0f;          // extra px between glyphs for legibility
    constexpr float kAnchorScale  = 1.6f;          // multiplier on kNodeRadius for the world-space anchor
    constexpr int   kPxClearance  = 4;             // extra px between projected sphere top and text baseline
    const Color     kLabelColor   = {255, 240, 220, 255};  // warm off-white, sits with the maroon/red palette
    const Color     kShadowColor  = {0,   0,   0,   190};  // 75% black, just enough to outline against the CRT noise
    Font const&     font          = GetFontDefault();

    const Vector3 cam_forward = Vector3Subtract(camera.target, camera.position);
    for (std::size_t i : indices) {
        if (i >= positions.size() || i >= stored_nodes.size()) continue;
        if (stored_nodes[i].deleted) continue;
        const auto& title = stored_nodes[i].title;
        if (title.empty()) continue;
        const Vector3 p     = positions[i];
        const Vector3 to_p  = Vector3Subtract(p, camera.position);
        if (Vector3DotProduct(to_p, cam_forward) <= 0.0f) continue;

        // Anchor point is the top of the sphere in world space: p shifted
        // along camera.up by kNodeRadius * kAnchorScale. Projecting THIS
        // (rather than p) means the pixel-space gap to the sphere stays
        // honest as zoom changes -- text scales with the rendered node
        // size, not the camera distance to the centre.
        const Vector3 anchor_world = Vector3Add(p,
            Vector3Scale(camera.up, kNodeRadius * kAnchorScale));
        const Vector2 anchor = GetWorldToScreen(anchor_world, camera);

        const Vector2 size   = MeasureTextEx(font, title.c_str(),
                                             static_cast<float>(kFontSize), kSpacing);
        const float   x      = anchor.x - size.x * 0.5f;
        const float   y      = anchor.y - size.y - static_cast<float>(kPxClearance);

        // Single-pixel shadow under the text to keep it legible against
        // both the dark void and the busier patches near edges + halos.
        // Drawn first so the foreground text sits on top.
        DrawTextEx(font, title.c_str(),
                   Vector2{x + 1.0f, y + 1.0f},
                   static_cast<float>(kFontSize), kSpacing, kShadowColor);
        DrawTextEx(font, title.c_str(),
                   Vector2{x, y},
                   static_cast<float>(kFontSize), kSpacing, kLabelColor);
    }
}

}  // namespace zg::render
