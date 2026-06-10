#pragma once

#include <string>
#include <vector>

namespace zg::ui {

// State for the CLI tab: scrollback lines plus the prompt's edit buffer.
// Owned by main so it survives tab switches.
struct CliState {
    std::vector<std::string> scrollback;
    std::string              input;
};

// Pure command dispatch, split from the ImGui layer so it's unit-testable.
// Takes one submitted line, appends the echo + response lines to
// `scrollback`, and returns true exactly when the line was /panic — the
// caller owns the actual wipe + exit. Unknown commands and blank lines
// never return true.
bool run_cli_command(const std::string& line,
                     std::vector<std::string>& scrollback);

// Renders the contents of the "CLI" tab (caller has already done
// ImGui::BeginTabItem / EndTabItem): scrollback area + a "> " prompt.
// Returns true when the operator entered /panic; the caller is expected
// to secure-wipe all project data and exit.
bool render_cli_tab(CliState& cli);

}  // namespace zg::ui
