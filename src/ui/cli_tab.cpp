#include "ui/cli_tab.h"

#include <imgui.h>
#include <imgui_stdlib.h>

namespace zg::ui {

namespace {

std::string trimmed(const std::string& s) {
    const auto b = s.find_first_not_of(" \t");
    if (b == std::string::npos) return "";
    const auto e = s.find_last_not_of(" \t");
    return s.substr(b, e - b + 1);
}

}  // namespace

bool run_cli_command(const std::string& line,
                     std::vector<std::string>& scrollback) {
    const std::string cmd = trimmed(line);
    if (cmd.empty()) return false;

    scrollback.push_back("> " + cmd);

    if (cmd == "/panic") {
        scrollback.push_back("wiping all project data and exiting.");
        return true;
    }
    if (cmd == "/help") {
        scrollback.push_back("/panic   overwrite + delete ALL project data, then exit");
        scrollback.push_back("/help    this list");
        return false;
    }
    scrollback.push_back("unknown command: " + cmd + " (try /help)");
    return false;
}

bool render_cli_tab(CliState& cli) {
    // Scrollback fills the tab except for one prompt line at the bottom.
    const float prompt_h = ImGui::GetFrameHeightWithSpacing();
    ImGui::BeginChild("cli_scrollback", ImVec2(0, -prompt_h));
    if (cli.scrollback.empty()) {
        ImGui::TextDisabled("zoigraph cli -- /help for commands");
    }
    for (const auto& l : cli.scrollback) ImGui::TextUnformatted(l.c_str());
    // Stick to the bottom while new lines arrive (the operator can still
    // scroll up; this only kicks in when already at the bottom).
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) {
        ImGui::SetScrollHereY(1.0f);
    }
    ImGui::EndChild();

    ImGui::TextUnformatted(">");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-FLT_MIN);
    bool fired = false;
    if (ImGui::InputText("##cli_input", &cli.input,
                         ImGuiInputTextFlags_EnterReturnsTrue)) {
        fired = run_cli_command(cli.input, cli.scrollback);
        cli.input.clear();
        // Keep the prompt focused after Enter so the operator can keep
        // typing — matches every terminal ever.
        ImGui::SetKeyboardFocusHere(-1);
    }
    return fired;
}

}  // namespace zg::ui
