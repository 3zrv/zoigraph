#include "persistence/project_store.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <system_error>

namespace zg::persistence {

namespace fs = std::filesystem;

bool is_valid_project_name(std::string_view name) {
    if (name.empty() || name.size() > 64) return false;
    for (char c : name) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (!std::isalnum(u) && c != '-' && c != '_') return false;
    }
    return true;
}

fs::path project_path(const fs::path& dir, std::string_view name) {
    return dir / (std::string(name) + ".db");
}

std::vector<std::string> list_projects(const fs::path& dir) {
    std::vector<std::string> out;
    std::error_code ec;
    if (!fs::exists(dir, ec) || !fs::is_directory(dir, ec)) return out;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (ec) break;
        if (!entry.is_regular_file()) continue;
        const auto& p = entry.path();
        const std::string fname = p.filename().string();
        if (!fname.empty() && fname.front() == '.') continue;
        if (p.extension() != ".db") continue;
        out.push_back(p.stem().string());
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::string read_last_project(const fs::path& dir, std::string_view fallback) {
    const fs::path last = dir / ".last";
    std::ifstream f(last);
    if (!f.is_open()) return std::string(fallback);
    std::string line;
    std::getline(f, line);
    // Trim trailing whitespace.
    while (!line.empty() &&
           std::isspace(static_cast<unsigned char>(line.back()))) {
        line.pop_back();
    }
    // Trim leading whitespace.
    std::size_t start = 0;
    while (start < line.size() &&
           std::isspace(static_cast<unsigned char>(line[start]))) {
        ++start;
    }
    line = line.substr(start);
    if (line.empty() || !is_valid_project_name(line)) return std::string(fallback);
    return line;
}

void write_last_project(const fs::path& dir, std::string_view name) {
    if (!is_valid_project_name(name)) return;
    std::error_code ec;
    fs::create_directories(dir, ec);
    std::ofstream f(dir / ".last", std::ios::trunc);
    if (!f.is_open()) return;
    f << name << '\n';
}

bool delete_project(const fs::path& dir, std::string_view name) {
    if (!is_valid_project_name(name)) return false;
    std::error_code ec;
    const fs::path db = project_path(dir, name);
    const bool removed = fs::remove(db, ec);
    // Best-effort cleanup of SQLite sidecars; ignore errors (they may not exist).
    for (const char* suffix : {"-journal", "-wal", "-shm"}) {
        std::error_code ec2;
        fs::remove(fs::path(db.string() + suffix), ec2);
    }
    return removed;
}

void migrate_legacy_db(const fs::path& legacy_path,
                       const fs::path& projects_dir,
                       std::string_view default_name) {
    std::error_code ec;
    fs::create_directories(projects_dir, ec);
    if (!is_valid_project_name(default_name)) return;
    if (!fs::exists(legacy_path, ec)) return;
    const fs::path dest = project_path(projects_dir, default_name);
    if (fs::exists(dest, ec)) return;  // never overwrite
    fs::rename(legacy_path, dest, ec);
    // Also try to move the SQLite sidecars next to the legacy file.
    for (const char* suffix : {"-journal", "-wal", "-shm"}) {
        std::error_code ec2;
        fs::rename(fs::path(legacy_path.string() + suffix),
                   fs::path(dest.string() + suffix), ec2);
    }
}

}  // namespace zg::persistence
