#include "telemetry/phantom_parse.h"

#include <nlohmann/json.hpp>

namespace zg::telemetry {

std::optional<Phantom> parse_phantom(std::string_view payload) {
    nlohmann::json j;
    try {
        j = nlohmann::json::parse(payload, nullptr, /*allow_exceptions=*/true);
    } catch (const nlohmann::json::parse_error&) {
        return std::nullopt;
    }

    if (!j.is_object()) return std::nullopt;
    if (!j.contains("id") || !j.contains("x") || !j.contains("y") || !j.contains("z")) {
        return std::nullopt;
    }
    if (!j["id"].is_number_integer()) return std::nullopt;
    if (!j["x"].is_number() || !j["y"].is_number() || !j["z"].is_number()) {
        return std::nullopt;
    }

    Phantom p{};
    p.id = j["id"].get<long long>();
    p.position = {
        j["x"].get<float>(),
        j["y"].get<float>(),
        j["z"].get<float>(),
    };
    if (j.contains("label") && j["label"].is_string()) {
        p.label = j["label"].get<std::string>();
    }
    if (j.contains("connections") && j["connections"].is_array()) {
        for (const auto& v : j["connections"]) {
            if (v.is_number_integer()) {
                p.connections.push_back(v.get<long long>());
            }
        }
    }
    p.spawn_time = 0.0;
    return p;
}

}  // namespace zg::telemetry
