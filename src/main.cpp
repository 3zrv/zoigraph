#include <raylib.h>
#include <raymath.h>

#include <imgui.h>
#include <rlImGui.h>

#include <cstddef>
#include <random>
#include <vector>

#include "graph/graph_buffer.h"
#include "graph/types.h"
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
    camera.position   = {60.0f, 60.0f, 60.0f};
    camera.target     = {0.0f, 0.0f, 0.0f};
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

    zg::graph::GraphBuffer buffer;
    zg::physics::PhysicsThread physics(
        make_initial_positions(kNodeCount),
        make_random_edges(kNodeCount, kEdgeCount),
        buffer);
    physics.start();

    std::vector<Vector3>         positions;
    std::vector<zg::graph::Edge> edges;
    std::vector<Matrix>          transforms;
    transforms.reserve(kNodeCount);

    int selected_node = -1;

    while (!WindowShouldClose()) {
        UpdateCamera(&camera, CAMERA_ORBITAL);
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
        ImGui::End();
        rlImGuiEnd();

        EndDrawing();
    }

    physics.stop();
    UnloadMesh(node_mesh);
    UnloadShader(instancing_shader);
    rlImGuiShutdown();
    CloseWindow();
    return 0;
}
