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
#include "app/clock.h"
#include "app/session.h"
#include "input/escape_wipe.h"
#include "macros/bones.h"
#include "macros/rabbit_hole.h"
#include "render/camera.h"
#include "render/draw.h"
#include "render/imgui_theme.h"
#include "render/shaders.h"
#include "ui/inspector_tab.h"
#include "ui/toolbar_tab.h"
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

using zg::app::unix_now;
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

    // All per-project state lives in Session; aliases below let the rest of
    // main.cpp keep using the short names while open_project repopulates
    // everything on each switch.
    zg::app::Session session;
    auto& db            = session.db;
    auto& physics       = session.physics;
    auto& stored_nodes  = session.stored_nodes;
    auto& edges         = session.edges;
    auto& positions     = session.positions;
    auto& cluster_ids   = session.cluster_ids;
    auto& tag_filter    = session.tag_filter;
    auto& self_idx      = session.self_idx;
    auto& timeline_mode = session.timeline_mode;
    auto& prev_open_ts  = session.prev_open_ts;
    auto& current_project = session.current_project;
    auto& selected_node = session.selected_node;
    auto& search_query  = session.search_query;
    auto& search_hits   = session.search_hits;

    std::vector<Matrix> transforms;
    transforms.reserve(512);

    auto open_project = [&](const std::string& name) {
        zg::app::open_project(session, name, kProjectsDir, buffer, phantom_buffer);
    };
    open_project(zg::persistence::read_last_project(kProjectsDir, "default"));

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
            zg::ui::render_inspector_tab(session, camera, phantoms, telemetry,
                                         show_grid, post_process);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Toolbar")) {
            zg::ui::render_toolbar_tab(session, phantom_buffer);
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
