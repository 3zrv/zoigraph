#include "render/scene.h"

#include <raylib.h>
#include <raymath.h>
#include <rlgl.h>

#include <algorithm>
#include <cstddef>
#include <functional>
#include <string>
#include <vector>

#include "app/clock.h"
#include "render/draw.h"
#include "render/sizes.h"

namespace zg::render {

void draw_scene_3d(const zg::app::Session& s,
                   const std::vector<zg::telemetry::Phantom>& phantoms,
                   const Camera3D& camera,
                   RenderTexture2D& scene_rt,
                   Mesh& node_mesh,
                   const Material& node_material,
                   const Material& node_material_dim,
                   const zg::macros::Bones& bones,
                   bool show_grid,
                   bool dim_filtered,
                   float phantom_ttl) {
    const auto& stored_nodes  = s.stored_nodes;
    const auto& edges         = s.edges;
    const auto& positions     = s.positions;
    const auto& cluster_ids   = s.cluster_ids;
    const auto& tag_filter    = s.tag_filter;
    const int   selected_node = s.selected_node;
    const double prev_open_ts = s.prev_open_ts;

    // When the operator has set a tag filter AND wants dimming, split
    // the per-frame transform list in two and issue two instanced draws
    // (dim material for non-matching, bright for matching) so the
    // matched subset visually pops out of the field. Otherwise one
    // batch with the bright material.
    const bool dim_active = dim_filtered && !tag_filter.empty();
    static std::vector<Matrix> transforms;
    static std::vector<Matrix> transforms_match;
    transforms.clear();
    transforms_match.clear();
    for (std::size_t i = 0; i < positions.size(); ++i) {
        // Soft-deleted nodes vanish from the field. Edges touching them
        // are also filtered out below so the trace doesn't dangle to
        // empty space.
        if (i < stored_nodes.size() && stored_nodes[i].deleted) continue;
        const Matrix m = MatrixTranslate(positions[i].x, positions[i].y, positions[i].z);
        if (dim_active && i < stored_nodes.size()) {
            const auto& tags = stored_nodes[i].tags;
            if (std::find(tags.begin(), tags.end(), tag_filter) != tags.end()) {
                transforms_match.push_back(m);
            } else {
                transforms.push_back(m);
            }
        } else {
            transforms.push_back(m);
        }
    }

    BeginTextureMode(scene_rt);
    ClearBackground(BLACK);

    BeginMode3D(camera);
    if (show_grid) DrawGrid(40, 5.0f);

    if (!transforms.empty()) {
        DrawMeshInstanced(node_mesh,
                          dim_active ? node_material_dim : node_material,
                          transforms.data(),
                          static_cast<int>(transforms.size()));
    }
    if (dim_active && !transforms_match.empty()) {
        DrawMeshInstanced(node_mesh, node_material,
                          transforms_match.data(),
                          static_cast<int>(transforms_match.size()));
    }

    // Edges with alpha keyed to certainty: confirmed/suspected/hearsay/
    // phantom fade progressively. Empty certainty (legacy edges) is
    // treated as confirmed.
    for (const auto& e : edges) {
        if (e.source >= positions.size() || e.target >= positions.size()) continue;
        // Skip edges with either endpoint tombstoned -- otherwise the
        // line dangles to invisible empty space.
        if (e.source < stored_nodes.size() && stored_nodes[e.source].deleted) continue;
        if (e.target < stored_nodes.size() && stored_nodes[e.target].deleted) continue;
        unsigned char alpha = 255;
        if      (e.certainty == "suspected") alpha = 180;
        else if (e.certainty == "hearsay")   alpha = 100;
        else if (e.certainty == "phantom")   alpha = 50;
        const Color line_color{MAROON.r, MAROON.g, MAROON.b, alpha};
        DrawLine3D(positions[e.source], positions[e.target], line_color);
    }

    // Per-node halos (tier / tag / cluster / tag-filter / diff) are
    // immediate-mode wire-spheres — one DrawSphereWires per node per active
    // halo. After auto-cluster every node gets a cluster halo, so this is N
    // draw calls and the perf cliff past instancing. Above kHaloCap nodes,
    // skip them: individual rings are visually indistinguishable in a field
    // that dense anyway. The self halo (a single node, navigationally useful)
    // always draws; so do the bounded selection/bones halos below the loop.
    // (Batching/instancing the halos would keep them at scale — deferred.)
    constexpr std::size_t kHaloCap = 4000;
    const bool draw_halos = positions.size() <= kHaloCap;

    // Tier indicators: every non-confirmed node gets a small wireframe
    // halo whose color reflects its tier. Confirmed nodes stay bare
    // (the bulk of the field) so the few tiered ones pop visually.
    // Self gets a bigger, always-visible green halo.
    for (std::size_t i = 0; i < positions.size() && i < stored_nodes.size(); ++i) {
        if (stored_nodes[i].deleted) continue;
        const auto& tier = stored_nodes[i].tier;
        if (tier == "self") {
            DrawSphereWires(positions[i], kNodeRadius * 2.0f, 12, 12, GREEN);
        } else if (draw_halos && tier == "suspected") {
            DrawSphereWires(positions[i], kNodeRadius * 1.4f, 8, 8, ORANGE);
        } else if (draw_halos && tier == "phantom") {
            DrawSphereWires(positions[i], kNodeRadius * 1.4f, 8, 8, VIOLET);
        }

        if (!draw_halos) continue;  // skip the remaining per-node halos at scale

        // Tag halo: nodes with at least one tag get an additional ring
        // colored by a hash of the first tag's name. Layered with the
        // tier halo above so both signals are readable.
        if (!stored_nodes[i].tags.empty()) {
            const std::string& tag = stored_nodes[i].tags.front();
            const std::size_t h = std::hash<std::string>{}(tag);
            const Color tag_col{
                static_cast<unsigned char>(0x60 | ((h >>  0) & 0x9F)),
                static_cast<unsigned char>(0x60 | ((h >>  8) & 0x9F)),
                static_cast<unsigned char>(0x60 | ((h >> 16) & 0x9F)),
                255,
            };
            DrawSphereWires(positions[i], kNodeRadius * 1.2f, 6, 6, tag_col);
        }

        // Cluster halo: when label-propagation has been run, draw an
        // outer ring colored by hash of the node's cluster id. Two
        // nodes in the same cluster share the same color.
        if (i < cluster_ids.size()) {
            const std::size_t h = std::hash<std::size_t>{}(cluster_ids[i] + 1);
            const Color cluster_col{
                static_cast<unsigned char>(0x70 | ((h >>  4) & 0x8F)),
                static_cast<unsigned char>(0x70 | ((h >> 12) & 0x8F)),
                static_cast<unsigned char>(0x70 | ((h >> 20) & 0x8F)),
                255,
            };
            DrawSphereWires(positions[i], kNodeRadius * 1.7f, 8, 8, cluster_col);
        }

        // Tag-filter highlight: bright cyan ring on any node carrying
        // the filtered tag. Nothing changes for non-matching nodes —
        // this is a highlight, not a hide.
        if (!tag_filter.empty()) {
            const auto& tags = stored_nodes[i].tags;
            if (std::find(tags.begin(), tags.end(), tag_filter) != tags.end()) {
                DrawSphereWires(positions[i], kNodeRadius * 2.2f, 10, 10, SKYBLUE);
            }
        }

        // Diff-since-last-open tints: nodes that appeared or changed
        // since the previous session get a temporary halo. NEW (created
        // in this session) trumps CHANGED so the bright color wins on
        // freshly-created nodes.
        if (prev_open_ts > 0.0) {
            const auto& sn = stored_nodes[i];
            if (sn.first_seen > prev_open_ts) {
                DrawSphereWires(positions[i], kNodeRadius * 1.8f, 10, 10,
                                Color{0, 220, 255, 220});  // bright cyan = NEW
            } else if (sn.last_touched > prev_open_ts) {
                DrawSphereWires(positions[i], kNodeRadius * 1.5f, 8, 8,
                                Color{255, 220, 80, 180}); // pale yellow = CHANGED
            }
        }
    }

    if (selected_node >= 0 && static_cast<std::size_t>(selected_node) < positions.size()) {
        DrawSphereWires(positions[selected_node], kNodeRadius * 1.6f, 10, 10, YELLOW);
    }

    // Bones halos: magenta wireframes around the 3 chosen nodes so the
    // operator can pick them out of the red field while the scratch
    // panel is open. Deliberately distinct from the yellow selection
    // halo above.
    if (bones.panel_open) {
        for (auto i : bones.chosen) {
            if (i >= positions.size()) continue;
            DrawSphereWires(positions[i], kNodeRadius * 1.9f, 10, 10, MAGENTA);
        }
    }

    // Phantom nodes: additive-blended glowing wireframes whose alpha
    // fades over the phantom TTL. Drawn after the static layer so the
    // glow accumulates against the dark background rather than mixing
    // with the red nodes. Phantoms carrying `connections` also render
    // animated jagged lines to each referenced Static Node.
    if (!phantoms.empty()) {
        rlSetBlendMode(RL_BLEND_ADDITIVE);
        // Phantom spawn_time is stamped with mono_now() (steady_clock) on the
        // telemetry/toolbar/CLI side, so the fade age MUST be measured against
        // the same clock — not raylib's GetTime() (a different epoch), which
        // would peg the glow at full alpha for the whole TTL. mono_now() also
        // works fine as the jagged-line animation phase.
        const double now = zg::app::mono_now();
        for (const auto& ph : phantoms) {
            const float age  = static_cast<float>(now - ph.spawn_time);
            const float life = std::clamp(1.0f - age / phantom_ttl, 0.0f, 1.0f);
            const Color glow{255, 200, 60, static_cast<unsigned char>(life * 255.0f)};
            DrawSphereWires(ph.position, kPhantomRadius, 6, 8, glow);

            for (const auto& c : ph.connections) {
                const auto idx = static_cast<std::size_t>(c.target);
                if (c.target < 0 || idx >= positions.size()) continue;
                if (idx < stored_nodes.size() && stored_nodes[idx].deleted) continue;
                draw_jagged_line(ph.position, positions[idx], glow, now, ph.id);
            }
        }
        rlSetBlendMode(RL_BLEND_ALPHA);
    }
    EndMode3D();
    EndTextureMode();
}

}  // namespace zg::render
