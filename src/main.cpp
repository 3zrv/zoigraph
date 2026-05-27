#include <raylib.h>
#include <raymath.h>
#include <rlgl.h>

#include <imgui.h>
#include <imgui_stdlib.h>
#include <rlImGui.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <ctime>
#include <random>
#include <set>
#include <vector>

#include "graph/cluster.h"
#include "graph/graph_buffer.h"
#include "graph/picks.h"
#include "graph/timeline.h"
#include "graph/types.h"
#include "graph/wikilinks.h"
#include "input/escape_wipe.h"
#include "macros/bones.h"
#include "macros/rabbit_hole.h"
#include "render/camera.h"
#include "render/draw.h"
#include "render/imgui_theme.h"
#include "render/shaders.h"
#include "persistence/db.h"
#include "persistence/project_store.h"
#include "persistence/seed.h"
#include "physics/physics_thread.h"
#include "telemetry/phantom.h"
#include "telemetry/phantom_buffer.h"
#include "telemetry/telemetry_thread.h"

#include <filesystem>
#include <memory>

namespace {

constexpr float kNodeRadius     = 0.5f;
constexpr int   kTelemetryPort  = 7777;
constexpr float kPhantomTtl     = 60.0f;   // seconds, per directive §5.B
constexpr float kPhantomRadius  = 1.2f;    // visibly larger than static nodes

using zg::render::kInstancingVS;
using zg::render::kInstancingFS;
using zg::render::kCrtFS;

// Wall-clock Unix seconds. Stable across process restarts (unlike raylib's
// GetTime which resets each launch), so it's what we persist for
// first_seen / last_touched.
double unix_now() {
    using namespace std::chrono;
    return duration<double>(system_clock::now().time_since_epoch()).count();
}

using zg::render::kCameraDefaultPos;
using zg::render::kCameraDefaultTarget;
using zg::render::update_orbit_camera;
using zg::macros::RabbitHole;
using zg::macros::pick_rabbit_path;
using zg::macros::update_rabbit_hole;
using zg::macros::Bones;
using zg::macros::throw_bones;
using zg::macros::update_bones;
using zg::macros::bones_fly_to_node;

}  // namespace

int main() {
    constexpr int kWidth  = 1280;
    constexpr int kHeight = 800;

    SetConfigFlags(FLAG_MSAA_4X_HINT | FLAG_WINDOW_RESIZABLE);
    InitWindow(kWidth, kHeight, "zoigraph");
    SetTargetFPS(144);
    // Raylib defaults to ESC-to-quit; disable so the triple-escape wipe
    // gets the keypresses instead.
    SetExitKey(KEY_NULL);

    rlImGuiSetup(true);
    zg::render::apply_terminal_theme();

    Camera3D camera{};
    camera.position   = kCameraDefaultPos;
    camera.target     = kCameraDefaultTarget;
    camera.up         = {0.0f, 1.0f, 0.0f};
    camera.fovy       = 60.0f;
    camera.projection = CAMERA_PERSPECTIVE;

    Shader instancing_shader = LoadShaderFromMemory(kInstancingVS, kInstancingFS);
    instancing_shader.locs[SHADER_LOC_MATRIX_MVP]   = GetShaderLocation(instancing_shader, "mvp");
    instancing_shader.locs[SHADER_LOC_MATRIX_MODEL] = GetShaderLocationAttrib(instancing_shader, "instanceTransform");

    Mesh node_mesh = GenMeshSphere(kNodeRadius, 8, 12);
    Material node_material = LoadMaterialDefault();
    node_material.shader = instancing_shader;
    node_material.maps[MATERIAL_MAP_DIFFUSE].color = RED;

    // CRT post-process pipeline: render the 3D scene into a texture, then
    // draw it back through the CRT shader. ImGui is layered on top of the
    // composite so the inspector stays crisp.
    RenderTexture2D scene_rt = LoadRenderTexture(kWidth, kHeight);
    Shader crt_shader = LoadShaderFromMemory(nullptr, kCrtFS);
    const int loc_crt_resolution = GetShaderLocation(crt_shader, "resolution");
    const int loc_crt_time       = GetShaderLocation(crt_shader, "time");

    // Multi-project model: each project lives in projects/<name>.db. The
    // legacy single-file zoigraph.db is migrated into projects/default.db
    // on first run with this code. The last-opened project name is stored
    // in projects/.last so the next launch resumes there.
    const std::filesystem::path kProjectsDir = "projects";
    zg::persistence::migrate_legacy_db("zoigraph.db", kProjectsDir, "default");

    zg::graph::GraphBuffer            buffer;
    zg::telemetry::PhantomBuffer      phantom_buffer;
    zg::telemetry::TelemetryThread    telemetry(kTelemetryPort, phantom_buffer);
    telemetry.start();

    std::unique_ptr<zg::persistence::Database>    db;
    std::unique_ptr<zg::physics::PhysicsThread>   physics;
    std::vector<zg::persistence::StoredNode>      stored_nodes;
    std::string                                   current_project;

    // Render-loop workspace — owned by main, NOT refilled from the buffer
    // each frame. Physics publishes positions; main owns edges. Edge
    // metadata edits (label / kind / certainty) live here and would be
    // clobbered if we snapshotted them back from the buffer.
    std::vector<Vector3>         positions;
    std::vector<zg::graph::Edge> edges;
    std::vector<Matrix>          transforms;
    std::vector<std::size_t>     cluster_ids;
    std::string                  tag_filter;
    std::size_t                  self_idx = SIZE_MAX;  // tier=="self" lookup, SIZE_MAX if absent
    bool                         timeline_mode  = false;
    double                       prev_open_ts   = 0.0;  // last_open_ts before this session bumped it
    transforms.reserve(512);

    auto open_project = [&](const std::string& name) {
        // Save and tear down the current session if there is one.
        if (physics) {
            physics->stop();
            std::vector<Vector3> final_positions;
            buffer.snapshot(final_positions);
            for (std::size_t i = 0; i < final_positions.size() && i < stored_nodes.size(); ++i) {
                stored_nodes[i].position = final_positions[i];
            }
            if (db) db->save_graph(stored_nodes, edges);
            physics.reset();
        }
        db.reset();
        phantom_buffer.clear();
        stored_nodes.clear();
        edges.clear();
        positions.clear();
        cluster_ids.clear();
        tag_filter.clear();

        current_project = name;
        zg::persistence::write_last_project(kProjectsDir, name);
        db = std::make_unique<zg::persistence::Database>(
            zg::persistence::project_path(kProjectsDir, name).string());

        std::vector<zg::graph::Edge> initial_edges;
        std::vector<Vector3>         initial_positions;
        if (db->load_graph(stored_nodes, initial_edges)) {
            initial_positions.reserve(stored_nodes.size());
            for (const auto& sn : stored_nodes) initial_positions.push_back(sn.position);
        } else {
            // Fresh project: named seed (self + alice + bob, ids 0..2)
            // followed by 300 random nodes (ids 3..302) populated with
            // codename titles, stock content snippets, 0-3 random tags
            // each, plus 200 random edges among them. Plenty of material
            // for auto-cluster / filter / search the moment the project
            // opens.
            const double now_ts = unix_now();
            auto seed = zg::persistence::make_initial_graph(now_ts);
            auto fill = zg::persistence::make_random_fill(
                /*node_count=*/300,
                /*edge_count=*/200,
                /*start_id=*/static_cast<long long>(seed.nodes.size()),
                /*now_unix=*/now_ts,
                /*spread=*/25.0f,
                /*rng_seed=*/42,
                /*with_data=*/true);
            stored_nodes = std::move(seed.nodes);
            stored_nodes.insert(stored_nodes.end(),
                                std::make_move_iterator(fill.nodes.begin()),
                                std::make_move_iterator(fill.nodes.end()));
            initial_edges = std::move(seed.edges);
            initial_edges.insert(initial_edges.end(),
                                 fill.edges.begin(), fill.edges.end());
            initial_positions.reserve(stored_nodes.size());
            for (const auto& sn : stored_nodes) initial_positions.push_back(sn.position);
        }
        // Seed main's render-loop state from what's about to be handed to
        // physics. From here on these belong to main.
        positions = initial_positions;
        edges     = initial_edges;

        // Find the self node (tier == "self") so touch-edges and the
        // self-pin both have a stable anchor. Falls back to SIZE_MAX if
        // the project doesn't have one (legacy DBs or operator-deleted).
        self_idx = SIZE_MAX;
        for (std::size_t i = 0; i < stored_nodes.size(); ++i) {
            if (stored_nodes[i].tier == "self") {
                self_idx = i;
                break;
            }
        }

        physics = std::make_unique<zg::physics::PhysicsThread>(
            std::move(initial_positions),
            initial_edges,
            buffer,
            &phantom_buffer);
        physics->start();

        // Pin the self node at its current position so the rest of the
        // graph arranges itself relative to a fixed center.
        if (self_idx < positions.size()) {
            physics->set_pin(self_idx, positions[self_idx]);
        }

        // Diff-since-last-open: snapshot the previous last_open_ts (used by
        // the render to tint new/changed nodes) then bump it to now. Old
        // projects without a stored value get prev_open_ts = 0, which
        // suppresses the tints on first viewing.
        prev_open_ts = db->meta_double("last_open_ts", 0.0);
        db->set_meta_double("last_open_ts", unix_now());

        // A project switch leaves any prior timeline-mode state behind —
        // start the new session in force-directed mode.
        timeline_mode = false;
    };

    // Ensure there's at least one project. If projects/ is empty, "default"
    // will be auto-created by open_project's fresh-DB branch.
    open_project(zg::persistence::read_last_project(kProjectsDir, "default"));

    int                              selected_node = -1;
    std::string                      search_query;
    std::vector<long long>           search_hits;
    std::vector<zg::telemetry::Phantom> phantoms;
    bool                             show_grid     = true;
    bool                             post_process  = true;
    RabbitHole                       rabbit;
    Bones                            bones;
    std::mt19937                     rabbit_rng(std::random_device{}());
    zg::input::EscapeWipe            esc_wipe;
    bool                             requested_exit = false;
    // Window for the triple-escape wipe. 1.5s is forgiving enough to survive
    // OS input latency on a relaxed tap-tap-tap; tightening this much under
    // 1s starts making the gesture feel finicky.
    constexpr double                 kWipeWindow = 1.5;

    while (!WindowShouldClose() && !requested_exit) {
        // Triple-Escape wipe — three ESC presses within kWipeWindow seconds
        // triggers a clean shutdown. (When SQLCipher lands this is where
        // the key buffer gets zeroed before the DB closes.)
        if (IsKeyPressed(KEY_ESCAPE) && esc_wipe.record(GetTime(), kWipeWindow)) {
            requested_exit = true;
        }

        if (IsWindowResized()) {
            UnloadRenderTexture(scene_rt);
            scene_rt = LoadRenderTexture(GetScreenWidth(), GetScreenHeight());
        }

        // H key triggers the Rabbit Hole macro from the currently selected
        // node. Ignored if nothing is selected or a macro is already running.
        const bool typing = ImGui::GetIO().WantTextInput;
        if (IsKeyPressed(KEY_H) && !typing && selected_node >= 0
            && static_cast<std::size_t>(selected_node) < positions.size()
            && !rabbit.active && !bones.active) {
            auto path = pick_rabbit_path(static_cast<std::size_t>(selected_node),
                                          edges, rabbit_rng, positions.size());
            if (path.size() >= 2) {
                rabbit.active        = true;
                rabbit.path          = std::move(path);
                rabbit.segment       = 0;
                rabbit.elapsed       = 0.0f;
                rabbit.camera_offset = Vector3Subtract(camera.position, camera.target);
            }
        }

        // B key throws the bones — three weakly-connected nodes, smooth fly
        // to frame all three, scratch panel asks what connects them.
        if (IsKeyPressed(KEY_B) && !typing && !rabbit.active && !bones.active
            && positions.size() >= 3) {
            throw_bones(bones, positions, edges, camera, rabbit_rng);
        }

        // T key toggles timeline mode: every node gets pinned at a
        // position derived from its first_seen on a horizontal axis;
        // pressing again unpins everything (except self) so physics
        // takes over again.
        if (IsKeyPressed(KEY_T) && !typing && physics && !stored_nodes.empty()) {
            timeline_mode = !timeline_mode;
            if (timeline_mode) {
                std::vector<double> firsts;
                firsts.reserve(stored_nodes.size());
                for (const auto& sn : stored_nodes) firsts.push_back(sn.first_seen);
                const auto layout = zg::graph::compute_timeline_positions(firsts);
                for (std::size_t i = 0; i < layout.size(); ++i) {
                    physics->set_pin(i, layout[i]);
                }
            } else {
                for (std::size_t i = 0; i < stored_nodes.size(); ++i) {
                    if (i != self_idx) physics->clear_pin(i);
                }
            }
        }

        if (rabbit.active) {
            update_rabbit_hole(rabbit, camera, positions, selected_node, GetFrameTime());
        } else if (bones.active) {
            update_bones(bones, camera, GetFrameTime());
        } else {
            update_orbit_camera(camera);
        }
        // Positions-only snapshot — edges are owned by main so the buffer
        // doesn't clobber operator edits to label / kind / certainty.
        buffer.snapshot(positions);
        phantom_buffer.snapshot_and_expire(phantoms, kPhantomTtl, GetTime());

        // Raypick on left-click, but only when ImGui isn't already eating the
        // mouse. Phantoms get first crack since they're bigger and ephemeral;
        // hitting one promotes it to a Static Node before the static-pick
        // pass even runs.
        if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && !ImGui::GetIO().WantCaptureMouse) {
            const Ray ray = GetScreenToWorldRay(GetMousePosition(), camera);

            int    phantom_hit = -1;
            float  phantom_dist = 0.0f;
            for (std::size_t i = 0; i < phantoms.size(); ++i) {
                const RayCollision hit = GetRayCollisionSphere(ray, phantoms[i].position, kPhantomRadius);
                if (hit.hit && (phantom_hit < 0 || hit.distance < phantom_dist)) {
                    phantom_dist = hit.distance;
                    phantom_hit  = static_cast<int>(i);
                }
            }

            if (phantom_hit >= 0) {
                // Promote: halt decay, append a Static Node carrying the
                // phantom's label as title, materialize any jagged-edge
                // connections as real graph edges, save to disk, queue
                // both node and edges into physics, then select. Promoted
                // node enters the "phantom" tier (visibly distinct from
                // confirmed Static Nodes).
                const auto& ph = phantoms[phantom_hit];
                const long long new_id = static_cast<long long>(stored_nodes.size());
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
                    const RayCollision hit = GetRayCollisionSphere(ray, positions[i], kNodeRadius);
                    if (hit.hit && (best_idx < 0 || hit.distance < best_dist)) {
                        best_dist = hit.distance;
                        best_idx  = static_cast<int>(i);
                    }
                }
                selected_node = best_idx;
            }
        }

        transforms.clear();
        for (const auto& p : positions) {
            transforms.push_back(MatrixTranslate(p.x, p.y, p.z));
        }

        // 3D pass renders into the off-screen RT so the CRT shader can post-
        // process it before ImGui draws on top.
        BeginTextureMode(scene_rt);
        ClearBackground(BLACK);

        BeginMode3D(camera);
        if (show_grid) DrawGrid(40, 5.0f);

        if (!transforms.empty()) {
            DrawMeshInstanced(node_mesh, node_material,
                              transforms.data(),
                              static_cast<int>(transforms.size()));
        }

        // Edges with alpha keyed to certainty: confirmed/suspected/hearsay/
        // phantom fade progressively. Empty certainty (legacy edges) is
        // treated as confirmed.
        for (const auto& e : edges) {
            if (e.source >= positions.size() || e.target >= positions.size()) continue;
            unsigned char alpha = 255;
            if      (e.certainty == "suspected") alpha = 180;
            else if (e.certainty == "hearsay")   alpha = 100;
            else if (e.certainty == "phantom")   alpha = 50;
            const Color line_color{MAROON.r, MAROON.g, MAROON.b, alpha};
            DrawLine3D(positions[e.source], positions[e.target], line_color);
        }

        // Tier indicators: every non-confirmed node gets a small wireframe
        // halo whose color reflects its tier. Confirmed nodes stay bare
        // (the bulk of the field) so the few tiered ones pop visually.
        // Self gets a bigger, always-visible green halo.
        for (std::size_t i = 0; i < positions.size() && i < stored_nodes.size(); ++i) {
            const auto& tier = stored_nodes[i].tier;
            if (tier == "self") {
                DrawSphereWires(positions[i], kNodeRadius * 2.0f, 12, 12, GREEN);
            } else if (tier == "suspected") {
                DrawSphereWires(positions[i], kNodeRadius * 1.4f, 8, 8, ORANGE);
            } else if (tier == "phantom") {
                DrawSphereWires(positions[i], kNodeRadius * 1.4f, 8, 8, VIOLET);
            }

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
            if (!tag_filter.empty() && i < stored_nodes.size()) {
                const auto& tags = stored_nodes[i].tags;
                if (std::find(tags.begin(), tags.end(), tag_filter) != tags.end()) {
                    DrawSphereWires(positions[i], kNodeRadius * 2.2f, 10, 10, SKYBLUE);
                }
            }

            // Diff-since-last-open tints: nodes that appeared or changed
            // since the previous session get a temporary halo. NEW (created
            // in this session) trumps CHANGED so the bright color wins on
            // freshly-created nodes.
            if (prev_open_ts > 0.0 && i < stored_nodes.size()) {
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
        // fades over the 60-second TTL. Drawn after the static layer so the
        // glow accumulates against the dark background rather than mixing
        // with the red nodes. Phantoms carrying `connections` also render
        // animated jagged lines to each referenced Static Node.
        if (!phantoms.empty()) {
            rlSetBlendMode(RL_BLEND_ADDITIVE);
            const double now = GetTime();
            for (const auto& ph : phantoms) {
                const float age = static_cast<float>(now - ph.spawn_time);
                const float life = std::clamp(1.0f - age / kPhantomTtl, 0.0f, 1.0f);
                const Color glow{255, 200, 60, static_cast<unsigned char>(life * 255.0f)};
                DrawSphereWires(ph.position, kPhantomRadius, 6, 8, glow);

                for (long long target_id : ph.connections) {
                    const auto idx = static_cast<std::size_t>(target_id);
                    if (target_id < 0 || idx >= positions.size()) continue;
                    zg::render::draw_jagged_line(ph.position, positions[idx], glow, now, ph.id);
                }
            }
            rlSetBlendMode(RL_BLEND_ALPHA);
        }
        EndMode3D();
        EndTextureMode();

        // Composite the 3D RT to the back buffer, optionally through the CRT
        // post-process. RenderTextures store flipped vertically — pass a
        // negative source height so the rendered image isn't upside down.
        BeginDrawing();
        ClearBackground(BLACK);

        const float scr_w = static_cast<float>(GetScreenWidth());
        const float scr_h = static_cast<float>(GetScreenHeight());
        if (post_process) {
            const Vector2 res{scr_w, scr_h};
            const float   t = static_cast<float>(GetTime());
            SetShaderValue(crt_shader, loc_crt_resolution, &res, SHADER_UNIFORM_VEC2);
            SetShaderValue(crt_shader, loc_crt_time,       &t,   SHADER_UNIFORM_FLOAT);
            BeginShaderMode(crt_shader);
        }
        const Rectangle src{0, 0,
                            static_cast<float>(scene_rt.texture.width),
                            -static_cast<float>(scene_rt.texture.height)};
        const Rectangle dst{0, 0, scr_w, scr_h};
        DrawTexturePro(scene_rt.texture, src, dst, {0, 0}, 0.0f, WHITE);
        if (post_process) {
            EndShaderMode();
        }

        // Edge labels — drawn AFTER the CRT composite so they stay crisp
        // and BEFORE ImGui so the inspector can sit on top of them. Skip
        // labels whose midpoint is behind the camera (GetWorldToScreen
        // returns nonsense for those).
        {
            const Vector3 cam_forward = Vector3Subtract(camera.target, camera.position);
            for (const auto& e : edges) {
                if (e.label.empty()) continue;
                if (e.source >= positions.size() || e.target >= positions.size()) continue;
                const Vector3 mid     = Vector3Lerp(positions[e.source], positions[e.target], 0.5f);
                const Vector3 to_mid  = Vector3Subtract(mid, camera.position);
                if (Vector3DotProduct(to_mid, cam_forward) <= 0.0f) continue;
                const Vector2 screen = GetWorldToScreen(mid, camera);
                const int tw = MeasureText(e.label.c_str(), 12);
                DrawText(e.label.c_str(),
                         static_cast<int>(screen.x) - tw / 2,
                         static_cast<int>(screen.y) - 7,
                         12, GRAY);
            }
        }

        rlImGuiBegin();

        // Main control panel — fixed-size, position remembered after the
        // first frame. Width clamps to fit the screen so on narrow displays
        // the panel never overflows; ImGui's built-in scrollbar handles
        // vertical overflow inside the tab content.
        const float main_w = std::min(380.0f, static_cast<float>(GetScreenWidth())  - 32.0f);
        const float main_h = std::min(720.0f, static_cast<float>(GetScreenHeight()) - 32.0f);
        ImGui::SetNextWindowPos (ImVec2(16, 16),       ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(main_w, main_h), ImGuiCond_Always);
        ImGui::Begin("// ZOIGRAPH //", nullptr, ImGuiWindowFlags_NoResize);
        ImGui::Text("zoigraph :: 0.0.0");
        ImGui::Separator();

        // ---- PROJECT switcher ------------------------------------------
        ImGui::TextDisabled("// PROJECT //");
        ImGui::Text("active: %s", current_project.c_str());
        {
            const auto names = zg::persistence::list_projects(kProjectsDir);
            int active_idx = -1;
            std::vector<const char*> ptrs;
            ptrs.reserve(names.size());
            for (std::size_t i = 0; i < names.size(); ++i) {
                ptrs.push_back(names[i].c_str());
                if (names[i] == current_project) active_idx = static_cast<int>(i);
            }
            if (!ptrs.empty()) {
                if (ImGui::Combo("switch", &active_idx, ptrs.data(),
                                 static_cast<int>(ptrs.size())) &&
                    active_idx >= 0 &&
                    names[static_cast<std::size_t>(active_idx)] != current_project) {
                    selected_node = -1;
                    search_query.clear();
                    search_hits.clear();
                    bones.panel_open = false;
                    rabbit.active = false;
                    open_project(names[static_cast<std::size_t>(active_idx)]);
                }
            }

            static std::string new_name;
            static std::string create_msg;
            const bool submitted = ImGui::InputText("new", &new_name,
                                                    ImGuiInputTextFlags_EnterReturnsTrue);
            if (ImGui::IsItemEdited()) create_msg.clear();
            ImGui::SameLine();
            const bool clicked = ImGui::SmallButton("create");
            if (submitted || clicked) {
                if (new_name.empty()) {
                    create_msg = "name is empty";
                } else if (!zg::persistence::is_valid_project_name(new_name)) {
                    create_msg = "name must be 1-64 chars: [A-Za-z0-9_-]";
                } else if (std::filesystem::exists(
                               zg::persistence::project_path(kProjectsDir, new_name))) {
                    create_msg = "project already exists";
                } else {
                    selected_node = -1;
                    search_query.clear();
                    search_hits.clear();
                    bones.panel_open = false;
                    rabbit.active = false;
                    const std::string to_open = new_name;
                    new_name.clear();
                    create_msg.clear();
                    open_project(to_open);
                }
            }
            if (!create_msg.empty()) {
                ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.5f, 1.0f), "%s", create_msg.c_str());
            }

            // Delete-current arming. First click sets the arm flag; second
            // confirms. Disables itself if there's only one project left
            // (always need at least one to be active).
            static bool delete_armed = false;
            const bool only_one = names.size() <= 1;
            if (only_one) ImGui::BeginDisabled();
            if (!delete_armed) {
                if (ImGui::SmallButton("delete current...")) delete_armed = true;
            } else {
                ImGui::TextColored(ImVec4(1, 0.3f, 0.3f, 1.0f), "click again to confirm");
                ImGui::SameLine();
                if (ImGui::SmallButton("yes, delete")) {
                    const std::string victim = current_project;
                    std::string fallback;
                    for (const auto& n : names) {
                        if (n != victim) { fallback = n; break; }
                    }
                    if (!fallback.empty()) {
                        selected_node = -1;
                        search_query.clear();
                        search_hits.clear();
                        bones.panel_open = false;
                        rabbit.active = false;
                        open_project(fallback);
                        zg::persistence::delete_project(kProjectsDir, victim);
                    }
                    delete_armed = false;
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("cancel")) delete_armed = false;
            }
            if (only_one) ImGui::EndDisabled();
        }
        ImGui::Separator();
        // ---- /PROJECT --------------------------------------------------

        if (ImGui::BeginTabBar("zg_tabs")) {
        if (ImGui::BeginTabItem("Inspector")) {

        ImGui::Text("nodes    %d", static_cast<int>(positions.size()));
        ImGui::Text("edges    %d", static_cast<int>(edges.size()));
        ImGui::Text("phantoms %d %s", static_cast<int>(phantoms.size()),
                    telemetry.listening() ? "(udp :7777)" : "(listener off)");
        ImGui::Text("fps      %d", GetFPS());
        ImGui::Separator();

        // Real-time FTS5 search: on each keystroke, re-query the DB and jump
        // camera + selection to the top hit. The DB save_graph happens on
        // edits/shutdown, so the index reflects the on-disk state — local
        // unsaved edits to title/content won't surface until "save to disk."
        if (ImGui::InputTextWithHint("search", "title or content", &search_query)) {
            search_hits = db->search(search_query);
            if (!search_hits.empty()) {
                const auto idx = static_cast<std::size_t>(search_hits.front());
                if (idx < positions.size()) {
                    selected_node = static_cast<int>(idx);
                    const Vector3 offset = Vector3Subtract(camera.position, camera.target);
                    camera.target = positions[idx];
                    camera.position = Vector3Add(camera.target, offset);
                }
            }
        }
        if (!search_query.empty()) {
            ImGui::TextDisabled("%d match%s",
                                static_cast<int>(search_hits.size()),
                                search_hits.size() == 1 ? "" : "es");
        }
        ImGui::Separator();
        if (selected_node >= 0 && static_cast<std::size_t>(selected_node) < positions.size()
            && static_cast<std::size_t>(selected_node) < stored_nodes.size()) {
            const Vector3 p = positions[selected_node];
            ImGui::Text("node %d   pos %+6.1f %+6.1f %+6.1f", selected_node, p.x, p.y, p.z);
            ImGui::Spacing();

            auto& sn = stored_nodes[selected_node];

            // Tier picker: drives halo color in the 3D view and persists
            // immediately on change (no save-to-disk required).
            static const char* kTiers[] = {"confirmed", "suspected", "phantom", "self"};
            int tier_idx = 0;
            for (int t = 0; t < 4; ++t) {
                if (sn.tier == kTiers[t]) { tier_idx = t; break; }
            }
            if (ImGui::Combo("tier", &tier_idx, kTiers, 4)) {
                sn.tier = kTiers[tier_idx];
                db->update_node_tier(sn.id, sn.tier);
            }

            // Tag chips: each existing tag renders as a small "<tag> x"
            // button. Clicking removes the tag and persists the new set.
            ImGui::TextDisabled("tags");
            int remove_idx = -1;
            for (std::size_t ti = 0; ti < sn.tags.size(); ++ti) {
                ImGui::PushID(static_cast<int>(ti));
                const std::string chip = sn.tags[ti] + " x";
                if (ImGui::SmallButton(chip.c_str())) {
                    remove_idx = static_cast<int>(ti);
                }
                ImGui::PopID();
                ImGui::SameLine();
            }
            ImGui::NewLine();
            if (remove_idx >= 0) {
                sn.tags.erase(sn.tags.begin() + remove_idx);
                db->update_node_tags(sn.id, sn.tags);
            }
            static std::string new_tag;
            const bool tag_submit = ImGui::InputText("add tag", &new_tag,
                                                     ImGuiInputTextFlags_EnterReturnsTrue);
            ImGui::SameLine();
            const bool tag_click = ImGui::SmallButton("+##tag");
            if (tag_submit || tag_click) {
                if (!new_tag.empty()) {
                    // Dedup before adding so the chip row doesn't grow
                    // visual duplicates that the DB would silently dedup.
                    if (std::find(sn.tags.begin(), sn.tags.end(), new_tag) == sn.tags.end()) {
                        sn.tags.push_back(new_tag);
                        db->update_node_tags(sn.id, sn.tags);
                    }
                    new_tag.clear();
                }
            }

            // Timestamps — read-only display. 0 means "unknown" (legacy row
            // from before the schema migration).
            if (sn.first_seen > 0.0) {
                const std::time_t t = static_cast<std::time_t>(sn.first_seen);
                char buf[32];
                std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", std::localtime(&t));
                ImGui::Text("first seen   %s", buf);
            } else {
                ImGui::TextDisabled("first seen   (unknown)");
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("set once when the node is created:\n"
                                  "  - fresh DB initial node generation\n"
                                  "  - click-to-pin promotion of a phantom\n"
                                  "never updated after that.");
            }
            if (sn.last_touched > 0.0) {
                const std::time_t t = static_cast<std::time_t>(sn.last_touched);
                char buf[32];
                std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", std::localtime(&t));
                ImGui::Text("last touched %s", buf);
            } else {
                ImGui::TextDisabled("last touched (unknown)");
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("bumped whenever the operator edits title or content.\n"
                                  "tier changes, physics drift, and incoming edges do NOT bump.");
            }
            ImGui::Spacing();

            bool text_changed = false;
            text_changed |= ImGui::InputText("title", &sn.title);
            ImGui::TextDisabled("content (markdown)");
            text_changed |= ImGui::InputTextMultiline(
                "##content", &sn.content,
                ImVec2(-FLT_MIN, ImGui::GetTextLineHeight() * 12));
            if (text_changed) {
                // Persist the edit immediately so search picks it up on the
                // next keystroke; triggers keep the FTS index in sync.
                const double now_ts = unix_now();
                sn.last_touched = now_ts;
                db->update_node_text(sn.id, sn.title, sn.content, now_ts);
                // The query may now have new hits.
                search_hits = db->search(search_query);

                // Touch-edge: any operator edit on a non-self node should
                // create a record-of-attention from self -> that node.
                // certainty="hearsay" so the line renders dim and doesn't
                // visually compete with confirmed edges.
                if (self_idx < stored_nodes.size() &&
                    self_idx != static_cast<std::size_t>(selected_node)) {
                    const std::size_t sel = static_cast<std::size_t>(selected_node);
                    bool already = false;
                    for (const auto& e : edges) {
                        if ((e.source == self_idx && e.target == sel) ||
                            (e.source == sel && e.target == self_idx)) {
                            already = true;
                            break;
                        }
                    }
                    if (!already) {
                        const zg::graph::Edge touch{
                            self_idx, sel, "touched", "saw-at", "hearsay"};
                        edges.push_back(touch);
                        physics->enqueue_edge(touch);
                    }
                }

                // Wikilinks: parse [[title]] occurrences out of the content
                // and materialize them as edges from this node to whichever
                // existing node has a matching title. Skip duplicates so
                // re-saving doesn't fan out parallel edges. Edge persists
                // via the explicit save button or shutdown save.
                const auto refs = zg::graph::extract_wikilinks(sn.content);
                for (const std::string& title : refs) {
                    if (title.empty()) continue;
                    std::size_t target_idx = SIZE_MAX;
                    for (std::size_t k = 0; k < stored_nodes.size(); ++k) {
                        if (stored_nodes[k].title == title) {
                            target_idx = k;
                            break;
                        }
                    }
                    if (target_idx == SIZE_MAX) continue;
                    if (target_idx == static_cast<std::size_t>(selected_node)) continue;
                    const std::size_t src_idx = static_cast<std::size_t>(selected_node);
                    bool exists = false;
                    for (const auto& e : edges) {
                        if ((e.source == src_idx && e.target == target_idx) ||
                            (e.source == target_idx && e.target == src_idx)) {
                            exists = true;
                            break;
                        }
                    }
                    if (exists) continue;
                    const zg::graph::Edge link_edge{src_idx, target_idx};
                    edges.push_back(link_edge);
                    physics->enqueue_edge(link_edge);
                }
            }

            if (ImGui::Button("save to disk")) {
                for (std::size_t i = 0;
                     i < positions.size() && i < stored_nodes.size(); ++i) {
                    stored_nodes[i].position = positions[i];
                }
                db->save_graph(stored_nodes, edges);
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("deselect")) selected_node = -1;

            // Incident edges — list every edge touching the selected node
            // with editable label / kind / certainty. Updates persist
            // immediately via update_edge; no save-button needed.
            ImGui::Spacing();
            ImGui::Separator();
            const std::size_t sel = static_cast<std::size_t>(selected_node);
            int incident_count = 0;
            for (const auto& e : edges) {
                if (e.source == sel || e.target == sel) ++incident_count;
            }
            ImGui::Text("edges (%d incident)", incident_count);

            // Edge editor — flat layout, no CollapsingHeader.  Each row is
            // its own ImGui::PushID scope so the label/kind/certainty
            // widgets get unique IDs per edge.  A persistent edit_counter
            // ticks every time a change registers; if you edit and the
            // counter doesn't move, the change isn't being detected.
            static const char* kKindLabels[]  = {"(none)", "knows", "owns", "saw-at", "shell-of", "suspected"};
            static const char* kKindValues[]  = {"",       "knows", "owns", "saw-at", "shell-of", "suspected"};
            static const char* kCertainties[] = {"confirmed", "suspected", "hearsay", "phantom"};
            static int edge_edit_counter = 0;
            ImGui::TextDisabled("edge edits registered: %d", edge_edit_counter);
            ImGui::Spacing();

            for (std::size_t i = 0; i < edges.size(); ++i) {
                auto& e = edges[i];
                if (e.source != sel && e.target != sel) continue;
                const std::size_t other = (e.source == sel) ? e.target : e.source;
                const char* other_title = (other < stored_nodes.size() && !stored_nodes[other].title.empty())
                                          ? stored_nodes[other].title.c_str()
                                          : "(untitled)";
                ImGui::PushID(static_cast<int>(i));
                ImGui::Separator();
                ImGui::Text("-> %zu  %s", other, other_title);

                bool changed = false;
                if (ImGui::InputText("label", &e.label))    changed = true;

                int kind_idx = 0;
                for (int k = 0; k < 6; ++k) if (e.kind == kKindValues[k]) { kind_idx = k; break; }
                if (ImGui::Combo("kind", &kind_idx, kKindLabels, 6)) {
                    e.kind = kKindValues[kind_idx];
                    changed = true;
                }

                int c_idx = 0;
                for (int k = 0; k < 4; ++k) if (e.certainty == kCertainties[k]) { c_idx = k; break; }
                if (ImGui::Combo("certainty", &c_idx, kCertainties, 4)) {
                    e.certainty = kCertainties[c_idx];
                    changed = true;
                }

                if (changed) {
                    ++edge_edit_counter;
                    db->update_edge(e.source, e.target, e.label, e.kind, e.certainty);
                }
                ImGui::PopID();
            }
        } else {
            ImGui::TextDisabled("selected: (none)");
            ImGui::TextDisabled("(left-click a node)");
        }
        ImGui::Separator();

        // ---- view filters ---------------------------------------------
        // Tag filter: collect the unique set of tags across all nodes and
        // expose them as a combo. Selecting one highlights matching nodes;
        // "(all)" clears the filter.
        {
            std::vector<std::string> unique_tags = {"(all)"};
            std::set<std::string>    seen;
            for (const auto& sn : stored_nodes) {
                for (const auto& t : sn.tags) {
                    if (seen.insert(t).second) unique_tags.push_back(t);
                }
            }
            int filter_idx = 0;
            for (std::size_t k = 1; k < unique_tags.size(); ++k) {
                if (unique_tags[k] == tag_filter) {
                    filter_idx = static_cast<int>(k);
                    break;
                }
            }
            std::vector<const char*> filter_ptrs;
            filter_ptrs.reserve(unique_tags.size());
            for (const auto& s : unique_tags) filter_ptrs.push_back(s.c_str());
            if (ImGui::Combo("filter by tag", &filter_idx,
                             filter_ptrs.data(),
                             static_cast<int>(filter_ptrs.size()))) {
                tag_filter = (filter_idx == 0) ? "" : unique_tags[static_cast<std::size_t>(filter_idx)];
            }
        }

        // Auto-cluster: button + clear button.  cluster_ids is the source of
        // truth; populated on demand, displayed by the cluster halo above.
        if (ImGui::Button("auto-cluster")) {
            cluster_ids = zg::graph::label_propagation(
                stored_nodes.size(), edges, /*max_iters=*/100);
        }
        ImGui::SameLine();
        if (ImGui::Button("clear clusters")) {
            cluster_ids.clear();
        }
        if (!cluster_ids.empty()) {
            std::set<std::size_t> unique_clusters(cluster_ids.begin(), cluster_ids.end());
            ImGui::TextDisabled("%zu clusters across %zu nodes",
                                unique_clusters.size(), cluster_ids.size());
        }

        ImGui::Separator();
        ImGui::Checkbox("show grid", &show_grid);
        ImGui::Checkbox("CRT post-process", &post_process);
        bool bh = physics->use_barnes_hut();
        if (ImGui::Checkbox("Barnes-Hut physics", &bh)) {
            physics->set_use_barnes_hut(bh);
        }
        ImGui::Separator();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Toolbar")) {
            // Toolbar tab — manual node creation and local phantom injection.
            // Same data paths used by click-to-pin and the UDP listener, so
            // operators get a quick way to grow the graph without leaving
            // the keyboard or running an external tool.

            static std::string tb_node_title;
            static std::string tb_node_msg;
            static int         tb_node_tier_idx = 0;
            static const char* kToolbarTiers[] = {"confirmed", "suspected", "phantom"};

            ImGui::TextDisabled("create static node");
            const bool node_submitted = ImGui::InputText("title##tb_node", &tb_node_title,
                                                         ImGuiInputTextFlags_EnterReturnsTrue);
            if (ImGui::IsItemEdited()) tb_node_msg.clear();
            ImGui::Combo("tier##tb_node", &tb_node_tier_idx, kToolbarTiers, 3);
            if (ImGui::Button("create node") || node_submitted) {
                if (tb_node_title.empty()) {
                    tb_node_msg = "title is empty";
                } else if (!physics || !db) {
                    tb_node_msg = "no active project";
                } else {
                    const double now_ts = unix_now();
                    const long long new_id = static_cast<long long>(stored_nodes.size());
                    const float angle = 0.7f * static_cast<float>(new_id);
                    const Vector3 spawn{
                        8.0f * std::cos(angle),
                        0.0f,
                        8.0f * std::sin(angle),
                    };
                    zg::persistence::StoredNode n{};
                    n.id           = new_id;
                    n.position     = spawn;
                    n.title        = tb_node_title;
                    n.content      = "";
                    n.first_seen   = now_ts;
                    n.last_touched = now_ts;
                    n.tier         = kToolbarTiers[tb_node_tier_idx];
                    stored_nodes.push_back(std::move(n));
                    physics->enqueue_node(spawn);
                    db->save_graph(stored_nodes, edges);
                    tb_node_msg = "added " + tb_node_title;
                    tb_node_title.clear();
                }
            }
            if (!tb_node_msg.empty()) {
                ImGui::TextDisabled("%s", tb_node_msg.c_str());
            }

            ImGui::Separator();

            static std::string tb_phantom_label;
            static std::string tb_phantom_msg;
            static long long   tb_phantom_id_counter = 1000000;

            ImGui::TextDisabled("inject phantom");
            const bool phantom_submitted = ImGui::InputText("label##tb_phantom", &tb_phantom_label,
                                                            ImGuiInputTextFlags_EnterReturnsTrue);
            if (ImGui::IsItemEdited()) tb_phantom_msg.clear();
            if (ImGui::Button("inject") || phantom_submitted) {
                zg::telemetry::Phantom p{};
                p.id         = tb_phantom_id_counter++;
                p.position   = {0.0f, 6.0f, 0.0f};
                p.label      = tb_phantom_label.empty() ? "(local)" : tb_phantom_label;
                p.spawn_time = GetTime();
                phantom_buffer.add(p);
                tb_phantom_msg = "injected id " + std::to_string(p.id);
                tb_phantom_label.clear();
            }
            if (!tb_phantom_msg.empty()) {
                ImGui::TextDisabled("%s", tb_phantom_msg.c_str());
            }

            ImGui::Separator();

            // Journal-as-nodes — timestamped first-class node with tag
            // "journal". Auto-edged from self (kind 'wrote') and also from
            // the currently-selected node if any (kind 'concerns') so the
            // entry threads itself into whatever the operator was looking
            // at when they wrote it. Memex trails by accident.
            static std::string tb_journal_text;
            static std::string tb_journal_msg;
            ImGui::TextDisabled("journal entry");
            ImGui::InputTextMultiline("##tb_journal", &tb_journal_text,
                                      ImVec2(-FLT_MIN, ImGui::GetTextLineHeight() * 4));
            if (ImGui::IsItemEdited()) tb_journal_msg.clear();
            if (ImGui::Button("save journal")) {
                if (tb_journal_text.empty()) {
                    tb_journal_msg = "entry is empty";
                } else if (!physics || !db) {
                    tb_journal_msg = "no active project";
                } else {
                    const double now_ts = unix_now();
                    const std::time_t tt = static_cast<std::time_t>(now_ts);
                    char tbuf[32];
                    std::strftime(tbuf, sizeof(tbuf), "journal-%Y%m%d-%H%M%S",
                                  std::localtime(&tt));
                    const long long new_id = static_cast<long long>(stored_nodes.size());
                    // Position near self so journal entries cluster around
                    // the operator; small angular offset by id keeps them
                    // from overlapping each other.
                    const float ang = 0.5f * static_cast<float>(new_id);
                    Vector3 anchor{0.0f, 0.0f, 0.0f};
                    if (self_idx < positions.size()) anchor = positions[self_idx];
                    const Vector3 spawn{
                        anchor.x + 3.0f * std::cos(ang),
                        anchor.y + 1.0f + 0.3f * static_cast<float>(new_id % 5),
                        anchor.z + 3.0f * std::sin(ang),
                    };
                    zg::persistence::StoredNode jn{};
                    jn.id           = new_id;
                    jn.position     = spawn;
                    jn.title        = tbuf;
                    jn.content      = tb_journal_text;
                    jn.first_seen   = now_ts;
                    jn.last_touched = now_ts;
                    jn.tier         = "confirmed";
                    jn.tags         = {"journal"};
                    stored_nodes.push_back(std::move(jn));
                    physics->enqueue_node(spawn);

                    // self -> journal edge (kind "wrote")
                    if (self_idx < stored_nodes.size()) {
                        const zg::graph::Edge e_self{
                            self_idx, static_cast<std::size_t>(new_id),
                            "wrote", "knows", "confirmed"};
                        edges.push_back(e_self);
                        physics->enqueue_edge(e_self);
                    }
                    // journal -> selected edge (kind "concerns") if a node
                    // is currently selected and isn't the journal itself.
                    if (selected_node >= 0 &&
                        static_cast<std::size_t>(selected_node) < stored_nodes.size() &&
                        static_cast<std::size_t>(selected_node) != static_cast<std::size_t>(new_id)) {
                        const zg::graph::Edge e_about{
                            static_cast<std::size_t>(new_id),
                            static_cast<std::size_t>(selected_node),
                            "concerns", "saw-at", "confirmed"};
                        edges.push_back(e_about);
                        physics->enqueue_edge(e_about);
                    }

                    db->save_graph(stored_nodes, edges);
                    tb_journal_msg = "saved " + std::string(tbuf);
                    tb_journal_text.clear();
                }
            }
            if (!tb_journal_msg.empty()) {
                ImGui::TextDisabled("%s", tb_journal_msg.c_str());
            }

            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Help")) {
            ImGui::TextDisabled("LEFT-CLICK         select node");
            ImGui::TextDisabled("RIGHT-DRAG         orbit");
            ImGui::TextDisabled("SHIFT+RIGHT-DRAG   pan");
            ImGui::TextDisabled("SCROLL WHEEL       zoom");
            ImGui::TextDisabled("R KEY              reset view");
            ImGui::TextDisabled("H KEY              rabbit hole");
            ImGui::TextDisabled("B KEY              throw the bones");
            ImGui::TextDisabled("T KEY              timeline collapse");
            ImGui::TextDisabled("ESC x 3            wipe + exit");
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
        }
        ImGui::End();

        // Bones scratch panel — separate ImGui window, opens when a throw
        // selects a triple and stays open until the operator closes it.
        // Each chosen-node row is a Selectable: clicking it smooth-flies the
        // camera to that node and updates the inspector selection.
        if (bones.panel_open) {
            // Place the bones panel next to the main window if there's room,
            // otherwise stack it below so it stays fully on-screen.
            const float scr_w = static_cast<float>(GetScreenWidth());
            const float scr_h = static_cast<float>(GetScreenHeight());
            const bool  side_by_side = scr_w > (main_w + 360.0f + 48.0f);
            const ImVec2 bones_pos  = side_by_side
                ? ImVec2(main_w + 32.0f, 16.0f)
                : ImVec2(16.0f, std::min(main_h + 32.0f, scr_h - 200.0f));
            const ImVec2 bones_size{
                std::min(360.0f, scr_w - bones_pos.x - 16.0f),
                std::min(320.0f, scr_h - bones_pos.y - 16.0f),
            };
            ImGui::SetNextWindowPos (bones_pos,  ImGuiCond_FirstUseEver);
            ImGui::SetNextWindowSize(bones_size, ImGuiCond_FirstUseEver);
            if (ImGui::Begin("// THROW THE BONES //", &bones.panel_open)) {
                ImGui::TextDisabled("what connects these?  (click a node to travel)");
                ImGui::Separator();
                for (std::size_t slot = 0; slot < bones.chosen.size(); ++slot) {
                    const auto i = bones.chosen[slot];
                    if (i >= stored_nodes.size()) continue;
                    const auto& sn = stored_nodes[i];
                    char row[320];
                    std::snprintf(row, sizeof(row), "[%zu] %s##bones-pick-%zu",
                                  i,
                                  sn.title.empty() ? "(untitled)" : sn.title.c_str(),
                                  slot);
                    if (ImGui::Selectable(row)) {
                        bones_fly_to_node(bones, i, positions, camera);
                        selected_node = static_cast<int>(i);
                    }
                }
                ImGui::Separator();
                ImGui::InputTextMultiline("##bones-scratch", &bones.scratch,
                                          ImVec2(-FLT_MIN, ImGui::GetTextLineHeight() * 10));
                if (ImGui::Button("throw again")) {
                    throw_bones(bones, positions, edges, camera, rabbit_rng);
                }
            }
            ImGui::End();
        }

        rlImGuiEnd();

        // Triple-escape wipe progress indicator — drawn last so it sits on
        // top of everything (ImGui included). Big and red so the operator
        // sees instantly whether their keypresses are landing.
        const int esc_recent = esc_wipe.count_recent(GetTime(), kWipeWindow);
        if (esc_recent > 0 && esc_recent < 3) {
            const char* msg = TextFormat("ESC %d/3", esc_recent);
            const int   tw  = MeasureText(msg, 36);
            DrawText(msg, GetScreenWidth() - tw - 24, 24, 36, RED);
        }

        EndDrawing();
    }

    telemetry.stop();
    if (physics) physics->stop();

    // Persist the converged layout for the currently-active project. Just
    // sync positions back into stored_nodes; don't rebuild ids or shrink
    // the vector — click-to-pin nodes are already in stored_nodes and
    // would be lost by a naive resize+id-assign.
    if (db) {
        std::vector<Vector3> final_positions;
        buffer.snapshot(final_positions);
        for (std::size_t i = 0; i < final_positions.size() && i < stored_nodes.size(); ++i) {
            stored_nodes[i].position = final_positions[i];
        }
        db->save_graph(stored_nodes, edges);
    }
    physics.reset();
    db.reset();

    UnloadRenderTexture(scene_rt);
    UnloadShader(crt_shader);
    UnloadMesh(node_mesh);
    UnloadShader(instancing_shader);
    rlImGuiShutdown();
    CloseWindow();
    return 0;
}
