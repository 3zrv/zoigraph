#include "app/settings.h"

#include <nlohmann/json.hpp>

#include <fstream>

namespace zg::app {

Settings load_settings(const std::filesystem::path& p) {
    Settings s;
    std::ifstream in(p);
    if (!in) return s;

    nlohmann::json j;
    try {
        j = nlohmann::json::parse(in);
    } catch (const nlohmann::json::parse_error&) {
        return s;
    }
    if (!j.is_object()) return s;

    if (j.contains("show_grid") && j["show_grid"].is_boolean()) {
        s.show_grid = j["show_grid"].get<bool>();
    }
    if (j.contains("post_process") && j["post_process"].is_boolean()) {
        s.post_process = j["post_process"].get<bool>();
    }
    if (j.contains("dim_filtered") && j["dim_filtered"].is_boolean()) {
        s.dim_filtered = j["dim_filtered"].get<bool>();
    }
    if (j.contains("telemetry_port") && j["telemetry_port"].is_number_integer()) {
        const int port = j["telemetry_port"].get<int>();
        if (port >= 1 && port <= 65535) s.telemetry_port = port;
    }
    return s;
}

bool save_settings(const std::filesystem::path& p, const Settings& s) {
    const nlohmann::json j = {
        {"show_grid",      s.show_grid},
        {"post_process",   s.post_process},
        {"dim_filtered",   s.dim_filtered},
        {"telemetry_port", s.telemetry_port},
    };
    std::ofstream out(p);
    if (!out) return false;
    out << j.dump(2) << '\n';
    return static_cast<bool>(out);
}

}  // namespace zg::app
