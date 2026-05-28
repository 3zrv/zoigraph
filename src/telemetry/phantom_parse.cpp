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
    // Two accepted shapes per entry, picked per-element so a single payload
    // can mix legacy and new forms:
    //   - bare integer            -> Connection{target=int, kind=""}
    //   - {"target": int, "kind": string}  -> full Connection
    // Anything else in the array (string, null, malformed object) is
    // silently dropped, matching the rest of the parser's permissive
    // policy on optional fields.
    if (j.contains("connections") && j["connections"].is_array()) {
        for (const auto& v : j["connections"]) {
            if (v.is_number_integer()) {
                p.connections.push_back(Connection{v.get<long long>(), ""});
            } else if (v.is_object() && v.contains("target")
                       && v["target"].is_number_integer()) {
                Connection c;
                c.target = v["target"].get<long long>();
                if (v.contains("kind") && v["kind"].is_string()) {
                    c.kind = v["kind"].get<std::string>();
                }
                p.connections.push_back(std::move(c));
            }
        }
    }
    if (j.contains("source") && j["source"].is_string()) {
        p.source = j["source"].get<std::string>();
    }
    if (j.contains("content") && j["content"].is_string()) {
        p.content = j["content"].get<std::string>();
    }
    p.spawn_time = 0.0;
    return p;
}

}  // namespace zg::telemetry
