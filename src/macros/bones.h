#pragma once

#include <raylib.h>

#include <cstddef>
#include <random>
#include <string>
#include <vector>

#include "graph/types.h"

namespace zg::macros {

// Throw-the-bones macro state (directive's schizoid layer). One instance
// lives in main; B key triggers a throw, the panel stays open until the
// operator dismisses it.
struct Bones {
    bool                     active     = false;
    bool                     panel_open = false;
    std::vector<std::size_t> chosen;
    std::string              scratch;
    float                    elapsed    = 0.0f;
    float                    duration   = 1.2f;
    Vector3                  from_target{};
    Vector3                  to_target{};
    Vector3                  from_position{};
    Vector3                  to_position{};
};

// Picks three weakly-connected nodes via zg::graph::pick_weakly_connected_triple,
// computes a framing position that fits all three, and arms the Bones
// state machine to smooth-fly the camera there. Opens the scratch panel.
void throw_bones(Bones& b,
                 const std::vector<Vector3>& positions,
                 const std::vector<graph::Edge>& edges,
                 const Camera3D& camera,
                 std::mt19937& rng);

// Advance the smooth fly by `dt` seconds. Snaps to the destination and
// clears `active` when complete. Panel state is left untouched.
void update_bones(Bones& b, Camera3D& camera, float dt);

// Re-triggers the Bones fly machinery to travel to a single node — used
// by the clickable rows inside the bones scratch panel. Preserves the
// current camera offset (zoom + angle).
void bones_fly_to_node(Bones& b,
                       std::size_t node_idx,
                       const std::vector<Vector3>& positions,
                       Camera3D& camera);

}  // namespace zg::macros
