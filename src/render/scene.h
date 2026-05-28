#pragma once

#include <raylib.h>

#include <vector>

#include "app/session.h"
#include "macros/bones.h"
#include "telemetry/phantom.h"

namespace zg::render {

// Renders one frame of the 3D scene into scene_rt (off-screen render
// texture so the CRT post-process can composite it back to the back
// buffer). Layers, in draw order:
//   1. optional ground grid
//   2. node bodies (1 or 2 instanced draws depending on tag-filter dim)
//   3. edges (alpha keyed to certainty)
//   4. per-node halos (tier / first-tag / cluster / tag-filter highlight /
//      since-last-open diff)
//   5. selection halo (yellow)
//   6. bones halos (magenta) when the scratch panel is open
//   7. phantom glow + jagged connections, additive-blended
//
// Pure-render: reads from session / phantoms / bones, writes only to the
// GPU and to its internal transform-vector scratch space (kept as
// function-local static across frames so we don't reallocate).
void draw_scene_3d(const zg::app::Session& session,
                   const std::vector<zg::telemetry::Phantom>& phantoms,
                   const Camera3D& camera,
                   RenderTexture2D& scene_rt,
                   Mesh& node_mesh,
                   const Material& node_material,
                   const Material& node_material_dim,
                   const zg::macros::Bones& bones,
                   bool show_grid,
                   bool dim_filtered,
                   float phantom_ttl);

}  // namespace zg::render
