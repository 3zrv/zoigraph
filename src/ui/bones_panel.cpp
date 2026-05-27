#include "ui/bones_panel.h"

#include <imgui.h>
#include <imgui_stdlib.h>
#include <raylib.h>

#include <algorithm>
#include <cstddef>
#include <cstdio>

namespace zg::ui {

void render_bones_panel(zg::macros::Bones& bones,
                        zg::app::Session& s,
                        Camera3D& camera,
                        float main_w,
                        float main_h,
                        std::mt19937& rng) {
    if (!bones.panel_open) return;

    auto& stored_nodes  = s.stored_nodes;
    auto& positions     = s.positions;
    auto& edges         = s.edges;
    auto& selected_node = s.selected_node;

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
                zg::macros::bones_fly_to_node(bones, i, positions, camera);
                selected_node = static_cast<int>(i);
            }
        }
        ImGui::Separator();
        ImGui::InputTextMultiline("##bones-scratch", &bones.scratch,
                                  ImVec2(-FLT_MIN, ImGui::GetTextLineHeight() * 10));
        if (ImGui::Button("throw again")) {
            zg::macros::throw_bones(bones, positions, edges, camera, rng);
        }
    }
    ImGui::End();
}

}  // namespace zg::ui
