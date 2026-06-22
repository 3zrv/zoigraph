#pragma once

#include <filesystem>

namespace zg::app {

// Process-scoped operator settings, persisted as a small JSON file next to
// the projects/ directory (NOT inside a project DB — these outlive any one
// project). Loaded once at startup; written by the CLI /set command and on
// clean exit so checkbox changes stick too. /panic wipes the file along
// with everything else.
struct Settings {
    bool show_grid      = true;   // reference ground plane
    bool post_process   = true;   // CRT shader composite
    bool dim_filtered   = true;   // dim non-matching nodes under a tag filter
    int  telemetry_port = 7777;   // UDP phantom listener port
    int  query_port     = 7778;   // UDP read query-channel listener port
    int  window_w       = 1280;   // window size: restored at launch, captured
    int  window_h       = 800;    // on clean exit, settable via /set size
};

// Bounds for window_w/window_h: anything outside is ignored on load and
// rejected by the CLI. Floor fits the fixed-size control panel; ceiling is
// 8K — beyond either, the value is a corruption, not a preference.
inline constexpr int kMinWindowW = 640,  kMaxWindowW = 7680;
inline constexpr int kMinWindowH = 480,  kMaxWindowH = 4320;

// Reads settings from `p`. Missing file, unparseable JSON, wrong-typed or
// absent keys, and out-of-range ports all fall back to the field defaults
// (per key, not all-or-nothing) — a corrupt settings file can never stop
// the app from launching.
Settings load_settings(const std::filesystem::path& p);

// Writes settings as pretty-printed JSON. Returns false if the file can't
// be written.
bool save_settings(const std::filesystem::path& p, const Settings& s);

}  // namespace zg::app
