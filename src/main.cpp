#include <raylib.h>
#include <raymath.h>

#include <imgui.h>
#include <rlImGui.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <random>
#include <vector>

#include "graph/graph_buffer.h"
#include "graph/types.h"
#include "persistence/db.h"
#include "physics/physics_thread.h"

namespace {

constexpr int   kNodeCount     = 500;
constexpr int   kEdgeCount     = 30;
constexpr float kInitialSpread = 20.0f;
constexpr int   kSeed          = 42;
constexpr float kNodeRadius    = 0.5f;

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

constexpr Vector3 kCameraDefaultPos    = {60.0f, 60.0f, 60.0f};
constexpr Vector3 kCameraDefaultTarget = {0.0f, 0.0f, 0.0f};

// Custom orbit camera: right-drag rotates around the target, middle-drag pans
// the target, scroll dollies, R resets. Gated against ImGui mouse-capture so
// dragging on the inspector doesn't reach through to the 3D layer.
void update_orbit_camera(Camera3D& camera) {
    const bool ui_has_mouse = ImGui::GetIO().WantCaptureMouse;
    const Vector2 dm        = GetMouseDelta();
    const float wheel       = GetMouseWheelMove();

    if (!ui_has_mouse) {
        if (IsMouseButtonDown(MOUSE_BUTTON_RIGHT) && (dm.x != 0.0f || dm.y != 0.0f)) {
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

        if (IsMouseButtonDown(MOUSE_BUTTON_MIDDLE) && (dm.x != 0.0f || dm.y != 0.0f)) {
            const Vector3 offset = Vector3Subtract(camera.position, camera.target);
            const Vector3 right  = Vector3Normalize(Vector3CrossProduct(camera.up, Vector3Negate(offset)));
            const Vector3 up     = Vector3Normalize(Vector3CrossProduct(Vector3Negate(offset), right));
            const float scale    = Vector3Length(offset) * 0.0015f;
            const Vector3 pan    = Vector3Add(Vector3Scale(right, -dm.x * scale),
                                              Vector3Scale(up,    dm.y * scale));
            camera.position = Vector3Add(camera.position, pan);
            camera.target   = Vector3Add(camera.target,   pan);
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
        initial_positions = make_initial_positions(kNodeCount);
        initial_edges     = make_random_edges(kNodeCount, kEdgeCount);
        stored_nodes.reserve(initial_positions.size());
        for (std::size_t i = 0; i < initial_positions.size(); ++i) {
            stored_nodes.push_back({static_cast<long long>(i), initial_positions[i], "", ""});
        }
    }

    zg::graph::GraphBuffer buffer;
    zg::physics::PhysicsThread physics(
        std::move(initial_positions),
        initial_edges,
        buffer);
    physics.start();

    std::vector<Vector3>         positions;
    std::vector<zg::graph::Edge> edges;
    std::vector<Matrix>          transforms;
    transforms.reserve(kNodeCount);

    int selected_node = -1;

    while (!WindowShouldClose()) {
        update_orbit_camera(camera);
        buffer.snapshot(positions, edges);

        // Raypick on left-click, but only when ImGui isn't already eating the
        // mouse. Uses last frame's WantCaptureMouse, which is fine — the panel
        // is opaque enough that the lag is invisible.
        if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && !ImGui::GetIO().WantCaptureMouse) {
            const Ray ray = GetScreenToWorldRay(GetMousePosition(), camera);
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

        transforms.clear();
        for (const auto& p : positions) {
            transforms.push_back(MatrixTranslate(p.x, p.y, p.z));
        }

        BeginDrawing();
        ClearBackground(BLACK);

        BeginMode3D(camera);
        DrawGrid(40, 5.0f);

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

        if (selected_node >= 0 && static_cast<std::size_t>(selected_node) < positions.size()) {
            DrawSphereWires(positions[selected_node], kNodeRadius * 1.6f, 10, 10, YELLOW);
        }
        EndMode3D();

        rlImGuiBegin();
        ImGui::SetNextWindowPos(ImVec2(16, 16), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(280, 180), ImGuiCond_FirstUseEver);
        ImGui::Begin("// INSPECTOR //");
        ImGui::Text("zoigraph :: 0.0.0");
        ImGui::Separator();
        ImGui::Text("nodes   %d", static_cast<int>(positions.size()));
        ImGui::Text("edges   %d", static_cast<int>(edges.size()));
        ImGui::Text("fps     %d", GetFPS());
        ImGui::Separator();
        if (selected_node >= 0 && static_cast<std::size_t>(selected_node) < positions.size()) {
            const Vector3 p = positions[selected_node];
            ImGui::Text("selected node %d", selected_node);
            ImGui::Text("  pos %+6.1f %+6.1f %+6.1f", p.x, p.y, p.z);
            if (ImGui::SmallButton("clear##sel")) selected_node = -1;
        } else {
            ImGui::TextDisabled("selected: (none)");
            ImGui::TextDisabled("(left-click a node)");
        }
        ImGui::Separator();
        ImGui::TextDisabled("L-CLICK  select");
        ImGui::TextDisabled("R-DRAG   orbit");
        ImGui::TextDisabled("M-DRAG   pan");
        ImGui::TextDisabled("WHEEL    zoom");
        ImGui::TextDisabled("R        reset view");
        ImGui::End();
        rlImGuiEnd();

        EndDrawing();
    }

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

    UnloadMesh(node_mesh);
    UnloadShader(instancing_shader);
    rlImGuiShutdown();
    CloseWindow();
    return 0;
}
