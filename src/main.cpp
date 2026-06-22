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
#include "app/settings.h"
#include "app/phantom_lifecycle.h"
#include "app/pick.h"
#include "app/paths.h"
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
#include "ui/cli_tab.h"
#include "ui/help_tab.h"
#include "ui/inspector_tab.h"
#include "ui/project_tab.h"
#include "ui/toolbar_tab.h"
#include "persistence/db.h"
#include "persistence/project_store.h"
#include "persistence/secure_wipe.h"
#include "persistence/seed.h"
#include "physics/physics_thread.h"
#include "app/query_responder.h"
#include "app/query_token.h"
#include "telemetry/phantom.h"
#include "telemetry/phantom_buffer.h"
#include "telemetry/query_protocol.h"
#include "telemetry/query_thread.h"
#include "telemetry/telemetry_thread.h"

#include <filesystem>
#include <memory>

namespace {

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
    // Operator settings (view flags, telemetry port, window size) persist
    // across runs in settings.json next to projects/. Loaded before the
    // window so launch restores the last size, and before the telemetry
    // thread so the listener binds the persisted port.
    const std::filesystem::path kSettingsPath = "settings.json";
    zg::app::Settings settings = zg::app::load_settings(kSettingsPath);

    SetConfigFlags(FLAG_MSAA_4X_HINT | FLAG_WINDOW_RESIZABLE);
    InitWindow(settings.window_w, settings.window_h, "zoigraph");
    SetTargetFPS(144);
    // Raylib defaults to ESC-to-quit; disable so the triple-escape exit
    // gesture gets the keypresses instead.
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
    RenderTexture2D scene_rt = LoadRenderTexture(settings.window_w,
                                                 settings.window_h);
    Shader crt_shader = LoadShaderFromMemory(nullptr, kCrtFS);
    zg::render::CrtShaderLocs crt_locs;
    crt_locs.resolution = GetShaderLocation(crt_shader, "resolution");
    crt_locs.time       = GetShaderLocation(crt_shader, "time");

    // Resolve resource dirs against the CWD first, then the executable's own
    // directory, so a relocated dist tarball (binary + scripts/ + projects/)
    // works when launched from another CWD. A dev run from the repo root keeps
    // using ./projects and ./scripts unchanged.
    const std::filesystem::path kCwdDir = std::filesystem::current_path();
    const char* kAppDirRaw = GetApplicationDirectory();
    const std::filesystem::path kExeDir = kAppDirRaw ? kAppDirRaw : "";

    // Multi-project model: each project lives in projects/<name>.db. The
    // legacy single-file zoigraph.db is migrated into projects/default.db
    // on first run with this code. The last-opened project name is stored
    // in projects/.last so the next launch resumes there.
    const std::filesystem::path kProjectsDir =
        zg::app::resolve_resource("projects", kCwdDir, kExeDir);
    zg::persistence::migrate_legacy_db("zoigraph.db", kProjectsDir, "default");

    zg::graph::GraphBuffer            buffer;
    zg::telemetry::PhantomBuffer      phantom_buffer;
    // unique_ptr (not a value) so the CLI /port command can tear the
    // listener down and rebind a fresh one mid-flight.
    auto telemetry = std::make_unique<zg::telemetry::TelemetryThread>(
        settings.telemetry_port, phantom_buffer);
    telemetry->start();

    // Read query channel: the LLM bridge asks the live graph for an anchor's
    // neighbourhood / search / a node, instead of reading the DB file directly
    // (which also keeps the SQLCipher swap-in clean later). Sibling loopback
    // socket on its own port; the render loop drains + answers it each frame.
    auto query_thread = std::make_unique<zg::telemetry::QueryThread>(
        settings.query_port);
    query_thread->start();

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
    // show_grid / post_process / dim_filtered live in `settings` (persisted);
    // show_all_labels stays ephemeral — it's a momentary overview toggle.
    bool                             show_all_labels  = false; // L toggles all-node titles overlay
    bool                             focus_inspector  = false; // set by double-click on a node, consumed by the Inspector tab
    zg::app::DoubleClickState        dbl_click;
    RabbitHole                       rabbit;
    Bones                            bones;

    // Phase-2 instrumentation: per-phantom spawn timestamps so the render
    // loop can diff each snapshot against last frame's set and emit
    // phantom_spawn / phantom_decay events into the active project DB.
    // handle_pick erases entries on a successful pin so the diff doesn't
    // misclassify pins as decays. tracker_project pins which project the
    // map is associated with so a project switch resets it without
    // logging spurious decays into the newly-opened DB. Keyed by name,
    // NOT by Database pointer: open_project frees the old Database and
    // allocates a same-sized replacement, so the allocator can hand back
    // the identical address and a pointer compare would silently miss
    // the switch.
    std::unordered_map<long long, double> seen_phantom_spawn;
    std::string                           tracker_project;

    // The lambda captures bones / rabbit so a project switch always closes
    // the bones scratch panel and cancels any in-flight rabbit hole. The
    // Session-owned resets (selected_node, search_*, etc.) happen inside
    // zg::app::open_project itself.
    // One auth secret per process for the read query channel; written 0600
    // beside each project DB on open so the LLM bridge can authenticate.
    session.query_token = zg::app::generate_session_token();
    // Point the Ask worker at the resolved emitter script (next to the binary
    // in a dist tarball, ./scripts in a dev run); it stats this before popen.
    session.ask.set_script_path(
        zg::app::resolve_resource("scripts/llm_phantom.py", kCwdDir, kExeDir).string());
    auto open_project = [&](const std::string& name) {
        bones.panel_open = false;
        rabbit.active    = false;
        zg::app::open_project(session, name, kProjectsDir, buffer, phantom_buffer);
        zg::app::write_token_file(
            zg::persistence::project_path(kProjectsDir, name).string() + ".token",
            session.query_token);
    };
    open_project(zg::persistence::read_last_project(kProjectsDir, "default"));
    std::mt19937                     rabbit_rng(std::random_device{}());
    zg::input::EscapeWipe            esc_exit;  // ESC x3 -> clean exit
    bool                             requested_exit = false;
    // Set by the CLI /panic command: skip the layout save, close every DB
    // handle, then secure-wipe all project data on the way out.
    bool                             requested_panic = false;
    zg::ui::CliState                 cli;
    zg::ui::CliDeps                  cli_deps;
    cli_deps.session        = &session;
    cli_deps.phantom_buffer = &phantom_buffer;
    cli_deps.camera         = &camera;
    cli_deps.phantoms       = &phantoms;
    cli_deps.projects_dir   = kProjectsDir;
    cli_deps.open_project   = open_project;
    cli_deps.get_port       = [&settings] { return settings.telemetry_port; };
    cli_deps.set_port       = [&](int port) {
        telemetry->stop();
        telemetry = std::make_unique<zg::telemetry::TelemetryThread>(
            port, phantom_buffer);
        telemetry->start();
        return true;  // bind lands async on the worker; poll listening()
    };
    cli_deps.port_listening = [&] { return telemetry->listening(); };
    cli_deps.get_query_port = [&settings] { return settings.query_port; };
    cli_deps.set_query_port = [&](int port) {
        query_thread->stop();
        query_thread = std::make_unique<zg::telemetry::QueryThread>(port);
        query_thread->start();
        return true;  // bind lands async on the worker; poll listening()
    };
    cli_deps.query_port_listening = [&] { return query_thread->listening(); };
    cli_deps.settings       = &settings;
    cli_deps.save_settings  = [&] {
        return zg::app::save_settings(kSettingsPath, settings);
    };
    cli_deps.spawn_tracker  = &seen_phantom_spawn;
    // The IsWindowResized() branch at the top of the frame loop re-creates
    // the scene render texture next frame, same as a drag-resize.
    cli_deps.set_window_size = [](int w, int h) { SetWindowSize(w, h); };
    cli_deps.ask_start      = [&](long long anchor_id) {
        session.ask.start(
            zg::persistence::project_path(kProjectsDir,
                                          session.current_project).string(),
            anchor_id);
    };
    // Window for the triple-escape exit gesture. 1.5s is forgiving enough to
    // survive OS input latency on a relaxed tap-tap-tap; tightening this much
    // under 1s starts making the gesture feel finicky.
    constexpr double                 kEscWindow = 1.5;

    while (!WindowShouldClose() && !requested_exit && !requested_panic) {
        if (IsWindowResized()) {
            UnloadRenderTexture(scene_rt);
            scene_rt = LoadRenderTexture(GetScreenWidth(), GetScreenHeight());
        }

        zg::app::handle_hotkeys(session, camera, esc_exit, kEscWindow,
                                rabbit, bones, rabbit_rng, requested_exit,
                                show_all_labels);

        // Query channel: answer the LLM bridge's reads (node / search /
        // neighbourhood) from our own live graph, on this thread — the socket
        // thread never touches graph data. drain() hands us the batch received
        // since last frame; answer_query enforces the auth token and tombstone
        // filtering, and a dropped (bad/empty-token) request gets no reply.
        {
            auto search_fn = [&](const std::string& text) -> std::vector<long long> {
                return db ? db->search(text) : std::vector<long long>{};
            };
            for (auto& iq : query_thread->drain()) {
                auto resp = zg::app::answer_query(iq.request, session.query_token,
                                                  stored_nodes, edges, search_fn);
                if (!resp) continue;
                query_thread->send_reply(
                    iq.reply_to, zg::telemetry::serialize_response(*resp));
            }
        }

        // Positions-only snapshot — edges are owned by main so the buffer
        // doesn't clobber operator edits to label / kind / certainty.
        buffer.snapshot(positions);
        phantom_buffer.snapshot_and_expire(phantoms, kPhantomTtl, zg::app::mono_now());

        // Cross-project guard: phantoms tagged for another project are
        // dropped BEFORE the spawn diff ever sees them. An Ask launched in
        // project A can land over UDP after the operator switched to B —
        // pinning it there would materialize edges against the wrong graph
        // and pollute B's events table. Untagged phantoms pass (legacy /
        // local senders). Reverse order so erase doesn't shift the
        // remaining indices.
        {
            const auto foreign = zg::app::foreign_phantom_indices(
                phantoms, session.current_project);
            for (auto it = foreign.rbegin(); it != foreign.rend(); ++it) {
                const auto& ph = phantoms[*it];
                if (db) {
                    nlohmann::json p = {
                        {"phantom_id",      ph.id},
                        {"phantom_project", ph.project},
                        {"active_project",  session.current_project},
                        {"source",          ph.source},
                    };
                    db->log_event("phantom_drop", -1, p.dump());
                }
                phantom_buffer.remove(ph.id);
                phantoms.erase(phantoms.begin()
                               + static_cast<std::ptrdiff_t>(*it));
            }
        }

        // Phantom lifecycle telemetry. Project-switch detection
        // first: if the active project changed, drop the tracker without
        // logging anything (those phantoms didn't really "decay" -- the
        // operator changed contexts). Then diff: new ids -> spawn event,
        // missing ids -> decay event (pins were already erased by
        // handle_pick before this block runs on the next frame -- but on
        // the SAME frame, the spawn we'd otherwise miss is also handled by
        // adding the phantom to the tracker BEFORE the pick call. Order
        // matters: snapshot -> spawn-diff -> pick.
        if (session.current_project != tracker_project) {
            seen_phantom_spawn.clear();
            tracker_project = session.current_project;
        }
        // Pure-logic diff in src/app/phantom_lifecycle.{h,cpp} (covered by
        // test_phantom_lifecycle); this loop just translates the delta
        // into log_event rows.
        const auto delta = zg::app::phantom_lifecycle_diff(
            seen_phantom_spawn, phantoms, zg::app::mono_now());
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

        // CLI /filter: phantoms outside the selected category disappear
        // from draw / pick / labels but stay in `phantoms`, so lifecycle
        // telemetry and the cross-project guard keep seeing the full set
        // (a hidden phantom still ages, decays, and logs truthfully).
        std::vector<zg::telemetry::Phantom> filtered_phantoms;
        if (!session.phantom_filter.empty()) {
            for (const auto& ph : phantoms) {
                if (ph.category == session.phantom_filter) {
                    filtered_phantoms.push_back(ph);
                }
            }
        }
        const auto& visible_phantoms =
            session.phantom_filter.empty() ? phantoms : filtered_phantoms;

        zg::app::handle_pick(session, camera, visible_phantoms, phantom_buffer,
                             dbl_click, focus_inspector, seen_phantom_spawn);

        zg::render::draw_scene_3d(session, visible_phantoms, camera, scene_rt,
                                  node_mesh, node_material, node_material_dim,
                                  bones, settings.show_grid,
                                  settings.dim_filtered, kPhantomTtl);

        BeginDrawing();
        ClearBackground(BLACK);
        zg::render::composite_scene(scene_rt, crt_shader, crt_locs,
                                    settings.post_process);
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
            session.selected_node, session.edges, visible_phantoms,
            bones_for_labels, session.stored_nodes, show_all_labels);
        zg::render::draw_node_labels(label_set, session.positions,
                                     session.stored_nodes, camera);

        rlImGuiBegin();

        // Main control panel — resizable by dragging the edges/corner;
        // position and size are remembered after the first frame. The
        // first-frame default clamps to the screen so the panel never
        // spawns overflowing a narrow display, and the per-frame max
        // constraint re-clamps it if the window later shrinks under it
        // (e.g. /set size).
        const float main_w = std::min(380.0f, static_cast<float>(GetScreenWidth())  - 32.0f);
        const float main_h = std::min(720.0f, static_cast<float>(GetScreenHeight()) - 32.0f);
        ImGui::SetNextWindowPos (ImVec2(16, 16),         ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(main_w, main_h), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSizeConstraints(
            ImVec2(280.0f, 240.0f),
            ImVec2(static_cast<float>(GetScreenWidth())  - 32.0f,
                   static_cast<float>(GetScreenHeight()) - 32.0f));
        ImGui::Begin("// ZOIGRAPH //");
        ImGui::Text("zoigraph :: 0.0.0");
        ImGui::Separator();

        if (ImGui::BeginTabBar("zg_tabs")) {
            if (ImGui::BeginTabItem("Project")) {
                zg::ui::render_project_tab(session, kProjectsDir, open_project,
                                           settings.dim_filtered,
                                           settings.show_grid,
                                           settings.post_process);
                ImGui::EndTabItem();
            }
            ImGuiTabItemFlags inspector_flags = ImGuiTabItemFlags_None;
            if (focus_inspector) {
                inspector_flags = ImGuiTabItemFlags_SetSelected;
                focus_inspector = false;
            }
            if (ImGui::BeginTabItem("Inspector", nullptr, inspector_flags)) {
                zg::ui::render_inspector_tab(session, camera, phantoms,
                                             *telemetry);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Toolbar")) {
                zg::ui::render_toolbar_tab(session, phantom_buffer);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("CLI")) {
                if (zg::ui::render_cli_tab(cli, cli_deps)) requested_panic = true;
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

        zg::render::draw_esc_hud(esc_exit, kEscWindow);

        EndDrawing();
    }

    telemetry->stop();
    query_thread->stop();
    if (physics) physics->stop();

    if (requested_panic) {
        // /panic: no layout save — release every handle so SQLite isn't
        // holding the files open, then overwrite + delete all project data
        // (every projects/*.db with WAL/SHM sidecars and the .last marker)
        // plus the pre-migration legacy DB if one is still around.
        physics.reset();
        db.reset();
        zg::persistence::panic_wipe(kProjectsDir);
        zg::persistence::secure_wipe_file("zoigraph.db");
        zg::persistence::secure_wipe_file(kSettingsPath);
    } else {
        // Persist the converged layout for the currently-active project.
        // Just sync positions back into stored_nodes; don't rebuild ids or
        // shrink the vector — click-to-pin nodes are already in
        // stored_nodes and would be lost by a naive resize+id-assign.
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
        // Checkbox/CLI changes both live in `settings`; one write on the
        // way out keeps the file current without per-frame IO. The window
        // size is captured here so a drag-resize sticks across runs (the
        // window is still open — CloseWindow comes after).
        settings.window_w = GetScreenWidth();
        settings.window_h = GetScreenHeight();
        zg::app::save_settings(kSettingsPath, settings);
    }

    UnloadRenderTexture(scene_rt);
    UnloadShader(crt_shader);
    UnloadMesh(node_mesh);
    UnloadShader(instancing_shader);
    rlImGuiShutdown();
    CloseWindow();
    return 0;
}
