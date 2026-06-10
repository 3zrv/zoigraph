#pragma once

#include <raylib.h>

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "app/session.h"
#include "app/settings.h"
#include "telemetry/phantom.h"
#include "telemetry/phantom_buffer.h"

namespace zg::ui {

// State for the CLI tab: scrollback lines plus the prompt's edit buffer.
// Owned by main so it survives tab switches.
struct CliState {
    std::vector<std::string> scrollback;
    std::string              input;
};

// Everything the CLI commands reach into, assembled once in main (all
// referents outlive the render loop). Pointers + std::function rather
// than hard links so test_cli can stub exactly the parts a command
// touches and leave the rest null — commands must check their deps and
// degrade with a scrollback message, never crash.
struct CliDeps {
    zg::app::Session*                          session        = nullptr;
    zg::telemetry::PhantomBuffer*              phantom_buffer = nullptr;
    Camera3D*                                  camera         = nullptr;
    const std::vector<zg::telemetry::Phantom>* phantoms       = nullptr;  // this frame's snapshot
    std::filesystem::path                      projects_dir;
    std::function<void(const std::string&)>    open_project;
    std::function<int()>                       get_port;
    std::function<bool(int)>                   set_port;        // restart listener on a new port
    std::function<bool()>                      port_listening;  // did the listener bind?
    zg::app::Settings*                         settings = nullptr;
    std::function<bool()>                      save_settings;   // flush settings to disk
};

// Splits a command line into tokens: whitespace-separated, with
// double-quoted spans kept as single tokens ("multi word title"). No
// escape sequences — a title containing a literal double-quote can't be
// typed, which is an accepted limitation. Unterminated quotes run to the
// end of the line. Pure; exposed for test_cli.
std::vector<std::string> cli_tokenize(const std::string& line);

// Pure command dispatch, split from the ImGui layer so it's unit-testable.
// Takes one submitted line, appends the echo + response lines to
// `scrollback`, and returns true exactly when the line was /panic — the
// caller owns the actual wipe + exit. Unknown commands and blank lines
// never return true.
bool run_cli_command(const std::string& line,
                     CliDeps& deps,
                     std::vector<std::string>& scrollback);

// Renders the contents of the "CLI" tab (caller has already done
// ImGui::BeginTabItem / EndTabItem): scrollback area + a "> " prompt.
// Returns true when the operator entered /panic; the caller is expected
// to secure-wipe all project data and exit.
bool render_cli_tab(CliState& cli, CliDeps& deps);

}  // namespace zg::ui
