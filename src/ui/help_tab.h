#pragma once

namespace zg::ui {

// Renders the contents of the "Help" tab (caller has already done
// ImGui::BeginTabItem / EndTabItem). Static cheat-sheet of mouse +
// keyboard bindings; takes no state because it never changes.
void render_help_tab();

}  // namespace zg::ui
