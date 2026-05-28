#pragma once

#include <filesystem>
#include <functional>
#include <string>

#include "app/session.h"

namespace zg::ui {

// Renders the contents of the "Project" tab (caller has already done
// ImGui::BeginTabItem / EndTabItem). Two sections:
//   - project switcher: active label, combo to switch, "new + create"
//     input, two-click "delete current" arm
//   - clustering: auto-cluster + clear buttons, count summary
//
// `open_project` is the caller-supplied callback that swaps to a given
// project by name. main() wraps zg::app::open_project plus the bones /
// rabbit panel resets in a lambda and passes it in here.
void render_project_tab(zg::app::Session& session,
                        const std::filesystem::path& projects_dir,
                        const std::function<void(const std::string&)>& open_project);

}  // namespace zg::ui
