#pragma once

#include <filesystem>
#include <string>

namespace zg::app {

// Resolve a resource path `name` (a file or directory, and `name` may itself
// contain subdirectories, e.g. "scripts/llm_phantom.py") against two roots,
// in order:
//   1. cwd_dir/name  -- a dev run from the repo root finds ./scripts, ./projects
//   2. exe_dir/name  -- a relocated dist tarball finds files next to the binary
//   3. cwd_dir/name  -- fresh run: unchanged from today's CWD-relative behaviour
// Returns the first of (1)/(2) that exists, otherwise the cwd_dir fallback.
// `exe_dir` may be empty (step 2 is skipped). Pure given the two roots.
std::filesystem::path resolve_resource(const std::string& name,
                                       const std::filesystem::path& cwd_dir,
                                       const std::filesystem::path& exe_dir);

}  // namespace zg::app
