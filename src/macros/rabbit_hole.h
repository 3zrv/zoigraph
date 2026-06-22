#pragma once

#include <raylib.h>

#include <cstddef>
#include <random>
#include <vector>

#include "graph/types.h"

namespace zg::macros {

inline constexpr float kRabbitSegmentDuration = 1.5f;  // seconds per hop
inline constexpr int   kRabbitHopCount        = 3;     // directive §5.C

// Per-session state for the Rabbit Hole macro. One instance lives in main;
// the macro is triggered by the H key with a node selected.
struct RabbitHole {
    bool                     active  = false;
    std::vector<std::size_t> path;            // node indices, length 2..(kRabbitHopCount + 1)
    int                      segment = 0;     // current animated segment in [0, path.size() - 1)
    float                    elapsed = 0.0f;  // seconds into the current segment
    Vector3                  camera_offset{}; // captured at trigger; preserved across the fly
};

// Walk `kRabbitHopCount` random connected edges starting at `start`. Pure
// function — no globals. May terminate early if a node has no neighbors,
// in which case the macro just animates a shorter trip.
//
// `alive` (empty == all alive) excludes tombstoned nodes: a deleted node is
// never hopped to, so the path never routes through one (the caller passes an
// alive `start`).
std::vector<std::size_t> pick_rabbit_path(std::size_t start,
                                          const std::vector<graph::Edge>& edges,
                                          std::mt19937& rng,
                                          std::size_t max_nodes,
                                          const std::vector<char>& alive = {});

// Advance an active RabbitHole by `dt` seconds. Smoothly interpolates the
// camera target along the current segment with smoothstep easing; on path
// completion promotes the selection to the final node and clears `active`.
void update_rabbit_hole(RabbitHole& rh,
                        Camera3D& camera,
                        const std::vector<Vector3>& positions,
                        int& selected_node,
                        float dt);

}  // namespace zg::macros
