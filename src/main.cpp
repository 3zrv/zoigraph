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
#include <vector>

#include "graph/graph_buffer.h"
#include "graph/picks.h"
#include "graph/types.h"
#include "graph/wikilinks.h"
#include "input/escape_wipe.h"
#include "persistence/db.h"
#include "physics/physics_thread.h"
#include "telemetry/phantom.h"
#include "telemetry/phantom_buffer.h"
#include "telemetry/telemetry_thread.h"

namespace {

constexpr int   kNodeCount      = 500;
constexpr int   kEdgeCount      = 30;
constexpr float kInitialSpread  = 20.0f;
constexpr int   kSeed           = 42;
constexpr float kNodeRadius     = 0.5f;
constexpr int   kTelemetryPort  = 7777;
constexpr float kPhantomTtl     = 60.0f;   // seconds, per directive §5.B
constexpr float kPhantomRadius  = 1.2f;    // visibly larger than static nodes

// Minimal GLSL 330 shader pair for raylib's DrawMeshInstanced. The vertex
// shader expects a per-instance mat4 named `instanceTransform`; raylib's
// renderer wires it up via SHADER_LOC_MATRIX_MODEL.
constexpr const char* kInstancingVS = R"GLSL(
#version 330
in vec3 vertexPosition;
in vec2 vertexTexCoord;
in vec3 vertexNormal;
in vec4 vertexColor;
in mat4 instanceTransform;

out vec2 fragTexCoord;
out vec4 fragColor;

uniform mat4 mvp;

void main() {
    fragTexCoord = vertexTexCoord;
    fragColor    = vertexColor;
    gl_Position  = mvp * instanceTransform * vec4(vertexPosition, 1.0);
}
)GLSL";

constexpr const char* kInstancingFS = R"GLSL(
#version 330
in vec2 fragTexCoord;
in vec4 fragColor;

uniform vec4 colDiffuse;

out vec4 finalColor;

void main() {
    finalColor = colDiffuse;
}
)GLSL";

// CRT post-process: chromatic aberration, scrolling scanlines, vignette.
// Operates over fragTexCoord (0..1) from a fullscreen draw of the scene
// RenderTexture. Resolution and time uniforms drive the per-frame look.
constexpr const char* kCrtFS = R"GLSL(
#version 330
in vec2 fragTexCoord;
in vec4 fragColor;

uniform sampler2D texture0;
uniform vec4 colDiffuse;
uniform vec2 resolution;
uniform float time;

out vec4 finalColor;

void main() {
    vec2 uv = fragTexCoord;
    vec2 center = vec2(0.5, 0.5);

    // Chromatic aberration: separate R / B sampling offsets, growing with
    // distance from screen center.
    vec2 ab = (uv - center) * 0.0035;
    float r = texture(texture0, uv + ab).r;
    float g = texture(texture0, uv).g;
    float b = texture(texture0, uv - ab).b;
    vec3 col = vec3(r, g, b);

    // Scanlines: vertical sinusoid in pixel space, slowly scrolling.
    float scan = sin((uv.y * resolution.y + time * 18.0) * 1.4) * 0.5 + 0.5;
    col *= mix(0.78, 1.0, scan);

    // Vignette: darken everything outside the central 60%.
    float d = length(uv - center);
    float vignette = smoothstep(0.85, 0.35, d);
    col *= vignette;

    finalColor = vec4(col, 1.0) * colDiffuse;
}
)GLSL";

std::vector<Vector3> make_initial_positions(int count) {
    std::mt19937 rng(kSeed);
    std::uniform_real_distribution<float> dist(-kInitialSpread, kInitialSpread);
    std::vector<Vector3> out;
    out.reserve(count);
    for (int i = 0; i < count; ++i) {
        out.push_back({dist(rng), dist(rng), dist(rng)});
    }
    return out;
}

std::vector<zg::graph::Edge> make_random_edges(int node_count, int edge_count) {
    std::mt19937 rng(kSeed ^ 0xBEEF);
    std::uniform_int_distribution<int> dist(0, node_count - 1);
    std::vector<zg::graph::Edge> out;
    out.reserve(edge_count);
    for (int i = 0; i < edge_count; ++i) {
        int a = dist(rng);
        int b = dist(rng);
        while (b == a) b = dist(rng);
        out.push_back({static_cast<std::size_t>(a), static_cast<std::size_t>(b)});
    }
    return out;
}

void apply_terminal_theme() {
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding    = 0.0f;
    style.FrameRounding     = 0.0f;
    style.ScrollbarRounding = 0.0f;
    style.WindowBorderSize  = 1.0f;
    style.FrameBorderSize   = 1.0f;
    style.WindowPadding     = ImVec2(8, 8);
    style.ItemSpacing       = ImVec2(6, 4);

    ImVec4* c = style.Colors;
    c[ImGuiCol_WindowBg]       = ImVec4(0.03f, 0.03f, 0.03f, 0.94f);
    c[ImGuiCol_Border]         = ImVec4(0.55f, 0.02f, 0.02f, 0.65f);
    c[ImGuiCol_TitleBg]        = ImVec4(0.10f, 0.00f, 0.00f, 1.00f);
    c[ImGuiCol_TitleBgActive]  = ImVec4(0.30f, 0.00f, 0.00f, 1.00f);
    c[ImGuiCol_TitleBgCollapsed] = ImVec4(0.05f, 0.00f, 0.00f, 1.00f);
    c[ImGuiCol_Text]           = ImVec4(0.95f, 0.25f, 0.25f, 1.00f);
    c[ImGuiCol_TextDisabled]   = ImVec4(0.45f, 0.10f, 0.10f, 1.00f);
    c[ImGuiCol_Separator]      = ImVec4(0.40f, 0.00f, 0.00f, 0.55f);
    c[ImGuiCol_FrameBg]        = ImVec4(0.08f, 0.00f, 0.00f, 0.80f);
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.18f, 0.00f, 0.00f, 0.80f);
    c[ImGuiCol_FrameBgActive]  = ImVec4(0.28f, 0.00f, 0.00f, 0.80f);
}

// Wall-clock Unix seconds. Stable across process restarts (unlike raylib's
// GetTime which resets each launch), so it's what we persist for
// first_seen / last_touched.
double unix_now() {
    using namespace std::chrono;
    return duration<double>(system_clock::now().time_since_epoch()).count();
}

constexpr Vector3 kCameraDefaultPos       = {60.0f, 60.0f, 60.0f};
constexpr Vector3 kCameraDefaultTarget    = {0.0f, 0.0f, 0.0f};
constexpr float   kRabbitSegmentDuration  = 1.5f;  // seconds per hop
constexpr int     kRabbitHopCount         = 3;     // directive §5.C: "through 3 random, connected edges"

// Rabbit Hole macro state (directive §5.C). Driven from main.cpp because it
// only ever exists in one instance and touches camera + selection state.
struct RabbitHole {
    bool                     active = false;
    std::vector<std::size_t> path;            // node indices, length 2..(kRabbitHopCount + 1)
    int                      segment = 0;     // current animated segment in [0, path.size() - 1)
    float                    elapsed = 0.0f;  // seconds into the current segment
    Vector3                  camera_offset{}; // captured at trigger time; preserved across the fly
};

// ---- Throw-the-bones macro -------------------------------------------------
// Picks three weakly-connected nodes, smoothly flies the camera to frame all
// three, and pops an ImGui scratch panel asking the operator what connects
// them. Forced pattern recognition.
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

void throw_bones(Bones& b,
                 const std::vector<Vector3>& positions,
                 const std::vector<zg::graph::Edge>& edges,
                 const Camera3D& camera,
                 std::mt19937& rng) {
    b.chosen = zg::graph::pick_weakly_connected_triple(positions.size(), edges, rng);
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
    // direction at a distance proportional to the spread, so all three fit
    // comfortably in frame.
    const Vector3 view_offset   = Vector3Subtract(camera.position, camera.target);
    const float   current_dist  = std::max(0.01f, Vector3Length(view_offset));
    const Vector3 dir_n         = Vector3Scale(view_offset, 1.0f / current_dist);
    const float   target_dist   = std::max(20.0f, spread * 2.5f);

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

// Re-triggers the Bones fly machinery for a single node — used by the
// clickable rows inside the bones scratch panel. Preserves the current
// camera offset (zoom + angle) so the view shifts to the node without
// reframing the whole field.
void bones_fly_to_node(Bones& b, std::size_t node_idx,
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

// Walk `kRabbitHopCount` random connected edges starting at `start`. May
// terminate early if a node has no neighbors — the macro just animates a
// shorter trip in that case.
std::vector<std::size_t> pick_rabbit_path(std::size_t start,
                                          const std::vector<zg::graph::Edge>& edges,
                                          std::mt19937& rng,
                                          std::size_t max_nodes) {
    std::vector<std::size_t> path = {start};
    std::size_t current = start;
    for (int hop = 0; hop < kRabbitHopCount; ++hop) {
        std::vector<std::size_t> neighbors;
        for (const auto& e : edges) {
            if (e.source == current && e.target < max_nodes && e.target != current) {
                neighbors.push_back(e.target);
            } else if (e.target == current && e.source < max_nodes && e.source != current) {
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

// Advance the macro by `dt` seconds; smoothly interpolate camera.target
// along the current segment with smoothstep easing. Promotes selection to
// the final node when the path completes.
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

// Animated jagged line for phantom-to-static connections. Subdivides the
// straight segment into kSegments pieces and perturbs each interior point
// perpendicular to the line using sin/cos driven by phantom-specific seed
// and a wall-clock time term, so the lines visibly jitter ("erratic" per
// directive §5.B).
void draw_jagged_line(Vector3 a, Vector3 b, Color color, double time, long long seed) {
    constexpr int   kSegments  = 8;
    constexpr float kAmplitude = 0.45f;

    const Vector3 dir       = Vector3Subtract(b, a);
    const float   length    = Vector3Length(dir);
    if (length < 0.01f) {
        DrawLine3D(a, b, color);
        return;
    }
    const Vector3 dir_n = Vector3Scale(dir, 1.0f / length);
    const Vector3 ref   = (std::fabs(dir_n.y) < 0.9f) ? Vector3{0, 1, 0} : Vector3{1, 0, 0};
    const Vector3 perp1 = Vector3Normalize(Vector3CrossProduct(dir_n, ref));
    const Vector3 perp2 = Vector3Normalize(Vector3CrossProduct(dir_n, perp1));

    Vector3 prev = a;
    for (int i = 1; i < kSegments; ++i) {
        const float t   = static_cast<float>(i) / kSegments;
        const float seg = static_cast<float>(i) * 37.0f
                        + static_cast<float>(seed % 997)
                        + static_cast<float>(time) * 7.0f;
        const float p1off = std::sin(seg)            * kAmplitude;
        const float p2off = std::cos(seg * 1.31f)    * kAmplitude;
        const Vector3 base   = Vector3Lerp(a, b, t);
        const Vector3 offset = Vector3Add(Vector3Scale(perp1, p1off),
                                          Vector3Scale(perp2, p2off));
        const Vector3 jagged = Vector3Add(base, offset);
        DrawLine3D(prev, jagged, color);
        prev = jagged;
    }
    DrawLine3D(prev, b, color);
}

// Custom orbit camera: right-drag rotates around the target, Shift+right-drag
// pans, scroll dollies, R resets. Gated against ImGui mouse-capture so
// dragging on the inspector doesn't reach through to the 3D layer.
void update_orbit_camera(Camera3D& camera) {
    const bool ui_has_mouse = ImGui::GetIO().WantCaptureMouse;
    const Vector2 dm        = GetMouseDelta();
    const float wheel       = GetMouseWheelMove();
    const bool shift_down   = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);

    if (!ui_has_mouse) {
        const bool dragging = IsMouseButtonDown(MOUSE_BUTTON_RIGHT) &&
                              (dm.x != 0.0f || dm.y != 0.0f);

        if (dragging && shift_down) {
            const Vector3 offset = Vector3Subtract(camera.position, camera.target);
            const Vector3 right  = Vector3Normalize(Vector3CrossProduct(camera.up, Vector3Negate(offset)));
            const Vector3 up     = Vector3Normalize(Vector3CrossProduct(Vector3Negate(offset), right));
            const float scale    = Vector3Length(offset) * 0.0015f;
            const Vector3 pan    = Vector3Add(Vector3Scale(right, -dm.x * scale),
                                              Vector3Scale(up,    dm.y * scale));
            camera.position = Vector3Add(camera.position, pan);
            camera.target   = Vector3Add(camera.target,   pan);
        } else if (dragging) {
            Vector3 offset = Vector3Subtract(camera.position, camera.target);

            // Yaw around the world up axis.
            offset = Vector3RotateByAxisAngle(offset, camera.up, -dm.x * 0.005f);

            // Pitch around the camera-right axis, clamped to avoid gimbal flip.
            const Vector3 right = Vector3Normalize(Vector3CrossProduct(camera.up, Vector3Negate(offset)));
            const Vector3 pitched = Vector3RotateByAxisAngle(offset, right, -dm.y * 0.005f);
            const Vector3 dir = Vector3Normalize(pitched);
            if (std::fabs(Vector3DotProduct(dir, camera.up)) < 0.985f) {
                offset = pitched;
            }
            camera.position = Vector3Add(camera.target, offset);
        }

        if (wheel != 0.0f) {
            Vector3 offset = Vector3Subtract(camera.position, camera.target);
            float distance = Vector3Length(offset);
            distance = std::clamp(distance * (1.0f - wheel * 0.1f), 2.0f, 500.0f);
            offset = Vector3Scale(Vector3Normalize(offset), distance);
            camera.position = Vector3Add(camera.target, offset);
        }
    }

    if (IsKeyPressed(KEY_R)) {
        camera.position = kCameraDefaultPos;
        camera.target   = kCameraDefaultTarget;
    }
}

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
    apply_terminal_theme();

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

    // Persistence: hydrate the graph from disk if a previous run saved one;
    // otherwise generate a fresh random layout. Node id == array index for
    // now — when deletion lands we'll need an id-to-index remap on load.
    zg::persistence::Database db("zoigraph.db");
    std::vector<zg::persistence::StoredNode> stored_nodes;
    std::vector<zg::graph::Edge>             initial_edges;
    std::vector<Vector3>                     initial_positions;

    if (db.load_graph(stored_nodes, initial_edges)) {
        initial_positions.reserve(stored_nodes.size());
        for (const auto& sn : stored_nodes) initial_positions.push_back(sn.position);
    } else {
        // Fresh DB: self node at id 0, anchored at origin, distinct tier.
        // Then kNodeCount random nodes at ids 1..N. Random edges shift up
        // by one so they reference the random nodes, never self.
        const double now_ts = unix_now();
        zg::persistence::StoredNode self_node{};
        self_node.id           = 0;
        self_node.position     = {0.0f, 0.0f, 0.0f};
        self_node.title        = "self";
        self_node.content      = "the operator";
        self_node.first_seen   = now_ts;
        self_node.last_touched = now_ts;
        self_node.tier         = "self";
        stored_nodes.push_back(std::move(self_node));
        initial_positions.push_back({0.0f, 0.0f, 0.0f});

        auto random_positions = make_initial_positions(kNodeCount);
        for (std::size_t i = 0; i < random_positions.size(); ++i) {
            initial_positions.push_back(random_positions[i]);
            zg::persistence::StoredNode n{};
            n.id           = static_cast<long long>(i + 1);
            n.position     = random_positions[i];
            n.first_seen   = now_ts;
            n.last_touched = now_ts;
            n.tier         = "confirmed";
            stored_nodes.push_back(std::move(n));
        }
        initial_edges = make_random_edges(kNodeCount, kEdgeCount);
        for (auto& e : initial_edges) {
            e.source += 1;
            e.target += 1;
        }
    }

    zg::graph::GraphBuffer            buffer;
    zg::telemetry::PhantomBuffer      phantom_buffer;
    zg::telemetry::TelemetryThread    telemetry(kTelemetryPort, phantom_buffer);
    zg::physics::PhysicsThread        physics(
        std::move(initial_positions),
        initial_edges,
        buffer,
        &phantom_buffer);
    physics.start();
    telemetry.start();

    std::vector<Vector3>         positions;
    std::vector<zg::graph::Edge> edges;
    std::vector<Matrix>          transforms;
    transforms.reserve(kNodeCount);

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

        if (rabbit.active) {
            update_rabbit_hole(rabbit, camera, positions, selected_node, GetFrameTime());
        } else if (bones.active) {
            update_bones(bones, camera, GetFrameTime());
        } else {
            update_orbit_camera(camera);
        }
        buffer.snapshot(positions, edges);
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
                    physics.enqueue_edge(new_edge);
                }

                phantom_buffer.remove(ph.id);
                db.save_graph(stored_nodes, edges);
                physics.enqueue_node(ph.position);
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

        for (const auto& e : edges) {
            if (e.source < positions.size() && e.target < positions.size()) {
                DrawLine3D(positions[e.source], positions[e.target], MAROON);
            }
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
                    draw_jagged_line(ph.position, positions[idx], glow, now, ph.id);
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

        rlImGuiBegin();
        ImGui::SetNextWindowPos(ImVec2(16, 16), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(360, 600), ImGuiCond_FirstUseEver);
        ImGui::Begin("// INSPECTOR //");
        ImGui::Text("zoigraph :: 0.0.0");
        ImGui::Separator();
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
            search_hits = db.search(search_query);
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
                db.update_node_tier(sn.id, sn.tier);
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
                db.update_node_text(sn.id, sn.title, sn.content, now_ts);
                // The query may now have new hits.
                search_hits = db.search(search_query);

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
                    physics.enqueue_edge(link_edge);
                }
            }

            if (ImGui::Button("save to disk")) {
                for (std::size_t i = 0;
                     i < positions.size() && i < stored_nodes.size(); ++i) {
                    stored_nodes[i].position = positions[i];
                }
                db.save_graph(stored_nodes, edges);
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("deselect")) selected_node = -1;
        } else {
            ImGui::TextDisabled("selected: (none)");
            ImGui::TextDisabled("(left-click a node)");
        }
        ImGui::Separator();
        ImGui::Checkbox("show grid", &show_grid);
        ImGui::Checkbox("CRT post-process", &post_process);
        bool bh = physics.use_barnes_hut();
        if (ImGui::Checkbox("Barnes-Hut physics", &bh)) {
            physics.set_use_barnes_hut(bh);
        }
        ImGui::Separator();
        ImGui::TextDisabled("LEFT-CLICK         select node");
        ImGui::TextDisabled("RIGHT-DRAG         orbit");
        ImGui::TextDisabled("SHIFT+RIGHT-DRAG   pan");
        ImGui::TextDisabled("SCROLL WHEEL       zoom");
        ImGui::TextDisabled("R KEY              reset view");
        ImGui::TextDisabled("H KEY              rabbit hole");
        ImGui::TextDisabled("B KEY              throw the bones");
        ImGui::TextDisabled("ESC x 3            wipe + exit");
        ImGui::End();

        // Bones scratch panel — separate ImGui window, opens when a throw
        // selects a triple and stays open until the operator closes it.
        // Each chosen-node row is a Selectable: clicking it smooth-flies the
        // camera to that node and updates the inspector selection.
        if (bones.panel_open) {
            ImGui::SetNextWindowPos(ImVec2(420, 16), ImGuiCond_FirstUseEver);
            ImGui::SetNextWindowSize(ImVec2(360, 320), ImGuiCond_FirstUseEver);
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
    physics.stop();

    // Persist the converged layout. Titles and content carry through from the
    // last load (or are empty on first run until the markdown editor lands).
    std::vector<Vector3>         final_positions;
    std::vector<zg::graph::Edge> final_edges;
    buffer.snapshot(final_positions, final_edges);
    stored_nodes.resize(final_positions.size());
    for (std::size_t i = 0; i < final_positions.size(); ++i) {
        stored_nodes[i].id       = static_cast<long long>(i);
        stored_nodes[i].position = final_positions[i];
    }
    db.save_graph(stored_nodes, final_edges);

    UnloadRenderTexture(scene_rt);
    UnloadShader(crt_shader);
    UnloadMesh(node_mesh);
    UnloadShader(instancing_shader);
    rlImGuiShutdown();
    CloseWindow();
    return 0;
}
