#include "macros/bones.h"

#include <raymath.h>

#include <algorithm>

#include "graph/picks.h"

namespace zg::macros {

void throw_bones(Bones& b,
                 const std::vector<Vector3>& positions,
                 const std::vector<graph::Edge>& edges,
                 const Camera3D& camera,
                 std::mt19937& rng,
                 const std::vector<char>& alive) {
    b.chosen = graph::pick_weakly_connected_triple(positions.size(), edges, rng, alive);
    if (b.chosen.size() != 3) {
        b.active = false;
        b.panel_open = false;
        return;
    }

    Vector3 centroid{0, 0, 0};
    for (auto i : b.chosen) {
        centroid.x += positions[i].x;
        centroid.y += positions[i].y;
        centroid.z += positions[i].z;
    }
    centroid.x /= 3.0f; centroid.y /= 3.0f; centroid.z /= 3.0f;

    float spread = 0.0f;
    for (std::size_t i = 0; i < b.chosen.size(); ++i) {
        for (std::size_t j = i + 1; j < b.chosen.size(); ++j) {
            const Vector3 d = Vector3Subtract(positions[b.chosen[i]], positions[b.chosen[j]]);
            spread = std::max(spread, Vector3Length(d));
        }
    }

    // Camera position: back off from the centroid along the current view
    // direction at a distance proportional to the spread, so all three
    // fit comfortably in frame.
    const Vector3 view_offset  = Vector3Subtract(camera.position, camera.target);
    const float   current_dist = std::max(0.01f, Vector3Length(view_offset));
    const Vector3 dir_n        = Vector3Scale(view_offset, 1.0f / current_dist);
    const float   target_dist  = std::max(20.0f, spread * 2.5f);

    b.from_target   = camera.target;
    b.to_target     = centroid;
    b.from_position = camera.position;
    b.to_position   = Vector3Add(centroid, Vector3Scale(dir_n, target_dist));
    b.elapsed       = 0.0f;
    b.active        = true;
    b.panel_open    = true;
    b.scratch.clear();
}

void update_bones(Bones& b, Camera3D& camera, float dt) {
    if (!b.active) return;
    b.elapsed += dt;
    if (b.elapsed >= b.duration) {
        camera.target   = b.to_target;
        camera.position = b.to_position;
        b.active        = false;
        return;
    }
    const float t = b.elapsed / b.duration;
    const float s = t * t * (3.0f - 2.0f * t);  // smoothstep
    camera.target   = Vector3Lerp(b.from_target,   b.to_target,   s);
    camera.position = Vector3Lerp(b.from_position, b.to_position, s);
}

void bones_fly_to_node(Bones& b,
                       std::size_t node_idx,
                       const std::vector<Vector3>& positions,
                       Camera3D& camera) {
    if (node_idx >= positions.size()) return;
    const Vector3 offset = Vector3Subtract(camera.position, camera.target);
    b.from_target   = camera.target;
    b.to_target     = positions[node_idx];
    b.from_position = camera.position;
    b.to_position   = Vector3Add(positions[node_idx], offset);
    b.elapsed       = 0.0f;
    b.duration      = 0.6f;  // snappier than the initial throw
    b.active        = true;
}

}  // namespace zg::macros
