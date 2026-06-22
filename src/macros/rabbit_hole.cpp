#include "macros/rabbit_hole.h"

#include <raymath.h>

namespace zg::macros {

std::vector<std::size_t> pick_rabbit_path(std::size_t start,
                                          const std::vector<graph::Edge>& edges,
                                          std::mt19937& rng,
                                          std::size_t max_nodes,
                                          const std::vector<char>& alive) {
    const auto is_alive = [&alive](std::size_t i) {
        return alive.empty() || (i < alive.size() && alive[i]);
    };
    std::vector<std::size_t> path = {start};
    std::size_t current = start;
    for (int hop = 0; hop < kRabbitHopCount; ++hop) {
        std::vector<std::size_t> neighbors;
        for (const auto& e : edges) {
            if (e.source == current && e.target < max_nodes && e.target != current
                && is_alive(e.target)) {
                neighbors.push_back(e.target);
            } else if (e.target == current && e.source < max_nodes && e.source != current
                       && is_alive(e.source)) {
                neighbors.push_back(e.source);
            }
        }
        if (neighbors.empty()) break;
        std::uniform_int_distribution<std::size_t> dist(0, neighbors.size() - 1);
        current = neighbors[dist(rng)];
        path.push_back(current);
    }
    return path;
}

void update_rabbit_hole(RabbitHole& rh,
                        Camera3D& camera,
                        const std::vector<Vector3>& positions,
                        int& selected_node,
                        float dt) {
    if (!rh.active) return;

    rh.elapsed += dt;
    while (rh.elapsed >= kRabbitSegmentDuration) {
        rh.elapsed -= kRabbitSegmentDuration;
        rh.segment++;
        if (rh.segment >= static_cast<int>(rh.path.size()) - 1) {
            if (!rh.path.empty() && rh.path.back() < positions.size()) {
                camera.target   = positions[rh.path.back()];
                camera.position = Vector3Add(camera.target, rh.camera_offset);
                selected_node   = static_cast<int>(rh.path.back());
            }
            rh.active = false;
            return;
        }
    }

    const std::size_t a_idx = rh.path[rh.segment];
    const std::size_t b_idx = rh.path[rh.segment + 1];
    if (a_idx >= positions.size() || b_idx >= positions.size()) {
        rh.active = false;
        return;
    }
    const float t      = rh.elapsed / kRabbitSegmentDuration;
    const float smooth = t * t * (3.0f - 2.0f * t);  // ease in / ease out
    camera.target   = Vector3Lerp(positions[a_idx], positions[b_idx], smooth);
    camera.position = Vector3Add(camera.target, rh.camera_offset);
}

}  // namespace zg::macros
