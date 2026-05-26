#include <raylib.h>

int main() {
    constexpr int kWidth = 1280;
    constexpr int kHeight = 800;

    SetConfigFlags(FLAG_MSAA_4X_HINT | FLAG_WINDOW_RESIZABLE);
    InitWindow(kWidth, kHeight, "zoigraph");
    SetTargetFPS(144);

    Camera3D camera{};
    camera.position   = {30.0f, 30.0f, 30.0f};
    camera.target     = {0.0f, 0.0f, 0.0f};
    camera.up         = {0.0f, 1.0f, 0.0f};
    camera.fovy       = 60.0f;
    camera.projection = CAMERA_PERSPECTIVE;

    while (!WindowShouldClose()) {
        UpdateCamera(&camera, CAMERA_ORBITAL);

        BeginDrawing();
        ClearBackground(BLACK);

        BeginMode3D(camera);
        DrawGrid(20, 1.0f);
        EndMode3D();

        DrawText("zoigraph :: scaffold", 16, 16, 20, RED);
        DrawFPS(16, GetScreenHeight() - 28);

        EndDrawing();
    }

    CloseWindow();
    return 0;
}
