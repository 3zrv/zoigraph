#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace zg::persistence {

// Pure file-system layer for the multi-project model: each project is one
// SQLite DB file at <dir>/<name>.db; `<dir>/.last` records the most-recently
// opened project so the next launch can resume there.
//
// Every function takes the projects directory as an argument so tests can
// substitute a temp path. Names are validated separately via
// is_valid_project_name — callers should reject invalid names before they
// reach any of these.

// Allowed: ASCII alphanumeric + '-' + '_', length 1..64. Anything else
// (slashes, spaces, dots, unicode) is rejected so a name can never traverse
// outside the projects directory or collide with the `.last` sidecar.
bool is_valid_project_name(std::string_view name);

// Returns the absolute path <dir>/<name>.db. Caller is responsible for
// validating `name` first.
std::filesystem::path project_path(const std::filesystem::path& dir,
                                   std::string_view name);

// Enumerates every *.db file directly inside `dir`, sorted alphabetically.
// Hidden files (leading dot) are skipped, as are the sidecar `.last`
// pointer and anything else without a .db extension.
std::vector<std::string> list_projects(const std::filesystem::path& dir);

// Returns the contents of <dir>/.last with whitespace trimmed, or `fallback`
// if the file is missing, unreadable, empty, or contains an invalid name.
std::string read_last_project(const std::filesystem::path& dir,
                              std::string_view fallback);

// Writes `name` to <dir>/.last. No-op if the name is invalid.
void write_last_project(const std::filesystem::path& dir,
                        std::string_view name);

// Deletes <dir>/<name>.db plus the SQLite -journal/-wal/-shm sidecars.
// Returns true if the main .db file was actually removed.
bool delete_project(const std::filesystem::path& dir, std::string_view name);

// One-shot migration helper: if `legacy_path` exists and
// <projects_dir>/<default_name>.db does not, move the legacy file into the
// projects directory under the default name. Creates `projects_dir` if it
// doesn't exist. Idempotent — safe to call on every startup.
void migrate_legacy_db(const std::filesystem::path& legacy_path,
                       const std::filesystem::path& projects_dir,
                       std::string_view default_name);

}  // namespace zg::persistence
