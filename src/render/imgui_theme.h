#pragma once

namespace zg::render {

// Applies the project's paranoid red-on-black terminal color palette and
// the zero-rounding style to the current ImGui context. Call once after
// rlImGuiSetup.
void apply_terminal_theme();

}  // namespace zg::render
