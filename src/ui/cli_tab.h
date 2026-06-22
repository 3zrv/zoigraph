#pragma once

#include <raylib.h>

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
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

    // Up/Down command history. `history` holds submitted lines oldest-
    // first; `hist_pos` is the entry currently shown (-1 == editing a
    // fresh line); `stash` preserves the unsubmitted input from before
    // the first Up so Down-past-the-newest restores it.
    std::vector<std::string> history;
    int                      hist_pos = -1;
    std::string              stash;
};

// One Up/Down step through the history. dir is -1 (older) or +1 (newer).
// Returns the text the prompt should now show, or nullopt when the step
// does nothing (empty history, Up at the oldest, Down on a fresh line).
// `current` is the prompt's text right now — stashed on the first Up so
// stepping back down past the newest entry restores it. Pure on CliState;
// exposed for test_cli.
std::optional<std::string> cli_history_step(CliState& cli, int dir,
                                            const std::string& current);

// Records a submitted line into the history: blanks are skipped, an exact
// repeat of the most recent entry is deduped, and navigation state resets
// either way. Exposed for test_cli.
void cli_history_push(CliState& cli, const std::string& line);

// Tab completion over the command table. Only the command word completes —
// input must be a single token starting with '/'. `completed` is what the
// prompt should now contain (== input when nothing applies): the full
// command plus a trailing space on a unique match, the longest common
// prefix when ambiguous. `candidates` lists every matching command so the
// caller can print them when there's more than one. Pure; exposed for
// test_cli.
struct CliCompletion {
    std::string              completed;
    std::vector<std::string> candidates;
};
CliCompletion cli_complete(const std::string& input);

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
    std::function<int()>                       get_query_port;
    std::function<bool(int)>                   set_query_port;       // rebind the query channel
    std::function<bool()>                      query_port_listening; // did the query socket bind?
    zg::app::Settings*                         settings = nullptr;
    std::function<bool()>                      save_settings;   // flush settings to disk
    std::function<void(int, int)>              set_window_size; // live-resize the OS window
    // main's phase-2 spawn tracker (phantom id -> spawn ts). /pin passes it
    // through to pin_phantom so the pin doesn't misfire a decay event; null
    // degrades to time_to_pin_s = 0 (the pin itself still works).
    std::unordered_map<long long, double>*     spawn_tracker = nullptr;
    // Fires session.ask against a node id (main supplies the DB path).
    // A std::function (not a direct LlmAsk::start call) so test_cli can
    // stub it — the real thing TCP-probes Ollama and spawns a subprocess.
    std::function<void(long long)>             ask_start;
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
