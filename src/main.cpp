#include <raylib.h>
#include <raymath.h>

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

}  // namespace

int main() {
    constexpr int kWidth  = 1280;
    constexpr int kHeight = 800;

    SetConfigFlags(FLAG_MSAA_4X_HINT | FLAG_WINDOW_RESIZABLE);
    InitWindow(kWidth, kHeight, "zoigraph");
    SetTargetFPS(144);

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

    while (!WindowShouldClose()) {
        UpdateCamera(&camera, CAMERA_ORBITAL);
        buffer.snapshot(positions, edges);

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
        EndMode3D();

        DrawText("zoigraph :: scaffold", 16, 16, 20, RED);
        DrawText(TextFormat("nodes %d  edges %d", static_cast<int>(positions.size()),
                            static_cast<int>(edges.size())),
                 16, 40, 16, GRAY);
        DrawFPS(16, GetScreenHeight() - 28);

        EndDrawing();
    }

    physics.stop();
    UnloadMesh(node_mesh);
    UnloadShader(instancing_shader);
    CloseWindow();
    return 0;
}
