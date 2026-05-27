#include "ui/help_tab.h"

#include <imgui.h>

namespace zg::ui {

void render_help_tab() {
    ImGui::TextDisabled("LEFT-CLICK         select node");
    ImGui::TextDisabled("RIGHT-DRAG         orbit");
    ImGui::TextDisabled("SHIFT+RIGHT-DRAG   pan");
    ImGui::TextDisabled("SCROLL WHEEL       zoom");
    ImGui::TextDisabled("R KEY              reset view");
    ImGui::TextDisabled("H KEY              rabbit hole");
    ImGui::TextDisabled("B KEY              throw the bones");
    ImGui::TextDisabled("T KEY              timeline collapse");
    ImGui::TextDisabled("ESC x 3            wipe + exit");
}

}  // namespace zg::ui
