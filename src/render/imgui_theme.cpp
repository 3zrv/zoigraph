#include "render/imgui_theme.h"

#include <imgui.h>

namespace zg::render {

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
    c[ImGuiCol_WindowBg]         = ImVec4(0.03f, 0.03f, 0.03f, 0.94f);
    c[ImGuiCol_Border]           = ImVec4(0.55f, 0.02f, 0.02f, 0.65f);
    c[ImGuiCol_TitleBg]          = ImVec4(0.10f, 0.00f, 0.00f, 1.00f);
    c[ImGuiCol_TitleBgActive]    = ImVec4(0.30f, 0.00f, 0.00f, 1.00f);
    c[ImGuiCol_TitleBgCollapsed] = ImVec4(0.05f, 0.00f, 0.00f, 1.00f);
    c[ImGuiCol_Text]             = ImVec4(0.95f, 0.25f, 0.25f, 1.00f);
    c[ImGuiCol_TextDisabled]     = ImVec4(0.45f, 0.10f, 0.10f, 1.00f);
    c[ImGuiCol_Separator]        = ImVec4(0.40f, 0.00f, 0.00f, 0.55f);
    c[ImGuiCol_FrameBg]          = ImVec4(0.08f, 0.00f, 0.00f, 0.80f);
    c[ImGuiCol_FrameBgHovered]   = ImVec4(0.18f, 0.00f, 0.00f, 0.80f);
    c[ImGuiCol_FrameBgActive]    = ImVec4(0.28f, 0.00f, 0.00f, 0.80f);
}

}  // namespace zg::render
