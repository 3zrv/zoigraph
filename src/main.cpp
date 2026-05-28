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
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

#include "graph/cluster.h"
#include "graph/graph_buffer.h"
#include "graph/picks.h"
#include "graph/timeline.h"
#include "graph/types.h"
#include "graph/wikilinks.h"
#include "app/clock.h"
#include "app/hotkeys.h"
#include "app/phantom_lifecycle.h"
#include "app/pick.h"
#include "app/session.h"
#include "input/escape_wipe.h"
#include "macros/bones.h"
#include "macros/rabbit_hole.h"
#include "render/camera.h"
#include "render/composite.h"
#include "render/draw.h"
#include "render/imgui_theme.h"
#include "render/labels.h"
#include "render/scene.h"
#include "render/shaders.h"
#include "render/sizes.h"
#include "ui/bones_panel.h"
#include "ui/help_tab.h"
#include "ui/inspector_tab.h"
#include "ui/project_tab.h"
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

constexpr int   kTelemetryPort  = 7777;
constexpr float kPhantomTtl     = 60.0f;   // seconds, per directive §5.B

using zg::render::kInstancingVS;
using zg::render::kInstancingFS;
using zg::render::kCrtFS;
using zg::render::kCameraDefaultPos;
using zg::render::kCameraDefaultTarget;
using zg::macros::RabbitHole;
using zg::macros::Bones;

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

    Mesh node_mesh = GenMeshSphere(zg::render::kNodeRadius, 8, 12);
    Material node_material = LoadMaterialDefault();
    node_material.shader = instancing_shader;
    node_material.maps[MATERIAL_MAP_DIFFUSE].color = RED;
    // Companion material for tag-filter dimming: same shader, much darker
    // diffuse so non-matching bodies fade into the background without
    // disappearing entirely (still useful as context for the matches).
    Material node_material_dim = LoadMaterialDefault();
    node_material_dim.shader = instancing_shader;
    node_material_dim.maps[MATERIAL_MAP_DIFFUSE].color = Color{40, 8, 10, 255};

    // CRT post-process pipeline: render the 3D scene into a texture, then
    // draw it back through the CRT shader. ImGui is layered on top of the
    // composite so the inspector stays crisp.
    RenderTexture2D scene_rt = LoadRenderTexture(kWidth, kHeight);
    Shader crt_shader = LoadShaderFromMemory(nullptr, kCrtFS);
    zg::render::CrtShaderLocs crt_locs;
    crt_locs.resolution = GetShaderLocation(crt_shader, "resolution");
    crt_locs.time       = GetShaderLocation(crt_shader, "time");

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

    std::vector<zg::telemetry::Phantom> phantoms;
    bool                             show_grid        = true;
    bool                             post_process     = true;
    bool                             dim_filtered     = true;  // dim non-matching nodes when a tag filter is active
    bool                             show_all_labels  = false; // L toggles all-node titles overlay
    bool                             focus_inspector  = false; // set by double-click on a node, consumed by the Inspector tab
    zg::app::DoubleClickState        dbl_click;
    RabbitHole                       rabbit;
    Bones                            bones;

    // Phase-2 instrumentation: per-phantom spawn timestamps so the render
    // loop can diff each snapshot against last frame's set and emit
    // phantom_spawn / phantom_decay events into the active project DB.
    // handle_pick erases entries on a successful pin so the diff doesn't
    // misclassify pins as decays. tracker_db pins which project DB the
    // map is associated with so a project switch resets it without
    // logging spurious decays into the newly-opened DB.
    std::unordered_map<long long, double> seen_phantom_spawn;
    zg::persistence::Database*            tracker_db = nullptr;

    // The lambda captures bones / rabbit so a project switch always closes
    // the bones scratch panel and cancels any in-flight rabbit hole. The
    // Session-owned resets (selected_node, search_*, etc.) happen inside
    // zg::app::open_project itself.
    auto open_project = [&](const std::string& name) {
        bones.panel_open = false;
        rabbit.active    = false;
        zg::app::open_project(session, name, kProjectsDir, buffer, phantom_buffer);
    };
    open_project(zg::persistence::read_last_project(kProjectsDir, "default"));
    std::mt19937                     rabbit_rng(std::random_device{}());
    zg::input::EscapeWipe            esc_wipe;
    bool                             requested_exit = false;
    // Window for the triple-escape wipe. 1.5s is forgiving enough to survive
    // OS input latency on a relaxed tap-tap-tap; tightening this much under
    // 1s starts making the gesture feel finicky.
    constexpr double                 kWipeWindow = 1.5;

    while (!WindowShouldClose() && !requested_exit) {
        if (IsWindowResized()) {
            UnloadRenderTexture(scene_rt);
            scene_rt = LoadRenderTexture(GetScreenWidth(), GetScreenHeight());
        }

        zg::app::handle_hotkeys(session, camera, esc_wipe, kWipeWindow,
                                rabbit, bones, rabbit_rng, requested_exit,
                                show_all_labels);

        // Positions-only snapshot — edges are owned by main so the buffer
        // doesn't clobber operator edits to label / kind / certainty.
        buffer.snapshot(positions);
        phantom_buffer.snapshot_and_expire(phantoms, kPhantomTtl, GetTime());

        // Phantom lifecycle telemetry. Project-switch detection
        // first: if the DB pointer moved, drop the tracker without logging
        // anything (those phantoms didn't really "decay" -- the operator
        // changed contexts). Then diff: new ids -> spawn event, missing
        // ids -> decay event (pins were already erased by handle_pick
        // before this block runs on the next frame -- but on the SAME
        // frame, the spawn we'd otherwise miss is also handled by adding
        // the phantom to the tracker BEFORE the pick call. Order matters:
        // snapshot -> spawn-diff -> pick.
        if (db.get() != tracker_db) {
            seen_phantom_spawn.clear();
            tracker_db = db.get();
        }
        // Pure-logic diff in src/app/phantom_lifecycle.{h,cpp} (covered by
        // test_phantom_lifecycle); this loop just translates the delta
        // into log_event rows.
        const auto delta = zg::app::phantom_lifecycle_diff(
            seen_phantom_spawn, phantoms, GetTime());
        if (db) {
            for (std::size_t idx : delta.new_indices) {
                const auto& ph = phantoms[idx];
                nlohmann::json conns = nlohmann::json::array();
                for (const auto& c : ph.connections) {
                    conns.push_back({{"target", c.target}, {"kind", c.kind}});
                }
                nlohmann::json p = {
                    {"phantom_id",  ph.id},
                    {"label",       ph.label},
                    {"content",     ph.content},
                    {"x",           ph.position.x},
                    {"y",           ph.position.y},
                    {"z",           ph.position.z},
                    {"connections", conns},
                    {"source",      ph.source},
                };
                db->log_event("phantom_spawn", -1, p.dump());
            }
            for (const auto& [id, lifetime_s] : delta.departed) {
                nlohmann::json p = {
                    {"phantom_id", id},
                    {"lifetime_s", lifetime_s},
                };
                db->log_event("phantom_decay", -1, p.dump());
            }
        }

        zg::app::handle_pick(session, camera, phantoms, phantom_buffer,
                             dbl_click, focus_inspector, seen_phantom_spawn);

        zg::render::draw_scene_3d(session, phantoms, camera, scene_rt,
                                  node_mesh, node_material, node_material_dim,
                                  bones, show_grid, dim_filtered, kPhantomTtl);

        BeginDrawing();
        ClearBackground(BLACK);
        zg::render::composite_scene(scene_rt, crt_shader, crt_locs, post_process);
        zg::render::draw_edge_labels(session, camera);
        // Node titles for the in-focus set (selected + 1-hop + phantom
        // targets + bones triple), or every non-deleted node when L is
        // toggled on. Drawn AFTER the CRT composite so the text stays
        // crisp, and BEFORE ImGui so the inspector still sits on top.
        // The static empty_bones avoids materialising a temporary in the
        // panel-closed branch -- both arms of the ?: stay lvalue refs.
        static const std::vector<std::size_t> empty_bones;
        const std::vector<std::size_t>& bones_for_labels =
            bones.panel_open ? bones.chosen : empty_bones;
        const auto label_set = zg::render::compute_label_set(
            session.selected_node, session.edges, phantoms,
            bones_for_labels, session.stored_nodes, show_all_labels);
        zg::render::draw_node_labels(label_set, session.positions,
                                     session.stored_nodes, camera);

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

        if (ImGui::BeginTabBar("zg_tabs")) {
            if (ImGui::BeginTabItem("Project")) {
                zg::ui::render_project_tab(session, kProjectsDir, open_project,
                                           dim_filtered, show_grid, post_process);
                ImGui::EndTabItem();
            }
            ImGuiTabItemFlags inspector_flags = ImGuiTabItemFlags_None;
            if (focus_inspector) {
                inspector_flags = ImGuiTabItemFlags_SetSelected;
                focus_inspector = false;
            }
            if (ImGui::BeginTabItem("Inspector", nullptr, inspector_flags)) {
                zg::ui::render_inspector_tab(session, camera, phantoms, telemetry);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Toolbar")) {
                zg::ui::render_toolbar_tab(session, phantom_buffer);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Help")) {
                zg::ui::render_help_tab();
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
        ImGui::End();

        zg::ui::render_bones_panel(bones, session, camera, main_w, main_h, rabbit_rng);

        rlImGuiEnd();

        zg::render::draw_esc_hud(esc_wipe, kWipeWindow);

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
