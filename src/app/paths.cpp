#include "app/paths.h"

namespace zg::app {

std::filesystem::path resolve_resource(const std::string& name,
                                       const std::filesystem::path& cwd_dir,
                                       const std::filesystem::path& exe_dir) {
    namespace fs = std::filesystem;
    std::error_code ec;

    const fs::path cwd_path = cwd_dir / name;
    if (fs::exists(cwd_path, ec)) return cwd_path;

    if (!exe_dir.empty()) {
        const fs::path exe_path = exe_dir / name;
        if (fs::exists(exe_path, ec)) return exe_path;
    }
    return cwd_path;  // fresh run: create/use it under the CWD, as before
}

}  // namespace zg::app
