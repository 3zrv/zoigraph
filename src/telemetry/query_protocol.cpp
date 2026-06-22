#include "telemetry/query_protocol.h"

#include <algorithm>

#include <nlohmann/json.hpp>

namespace zg::telemetry {

namespace {

QueryRequest invalid(long long req, std::string why) {
    QueryRequest r;
    r.kind  = QueryRequest::Kind::Invalid;
    r.req   = req;
    r.error = std::move(why);
    return r;
}

}  // namespace

QueryRequest parse_query(std::string_view payload, std::size_t max_bytes) {
    if (payload.size() > max_bytes) return invalid(-1, "payload too large");

    nlohmann::json j;
    try {
        j = nlohmann::json::parse(payload, nullptr, /*allow_exceptions=*/true);
    } catch (const nlohmann::json::parse_error&) {
        return invalid(-1, "malformed JSON");
    }
    if (!j.is_object()) return invalid(-1, "payload is not a JSON object");

    // req is optional (echoed for client-side correlation); default -1.
    long long req = -1;
    if (j.contains("req") && j["req"].is_number_integer()) {
        req = j["req"].get<long long>();
    }

    if (!j.contains("q") || !j["q"].is_string()) {
        return invalid(req, "missing string field \"q\"");
    }
    const std::string q = j["q"].get<std::string>();

    QueryRequest r;
    r.req = req;
    if (j.contains("token") && j["token"].is_string()) {
        r.token = j["token"].get<std::string>();
    }

    if (q == "neighborhood") {
        if (!j.contains("id") || !j["id"].is_number_integer()) {
            return invalid(req, "neighborhood requires integer \"id\"");
        }
        r.kind = QueryRequest::Kind::Neighborhood;
        r.id   = j["id"].get<long long>();
        r.hops = 1;
        if (j.contains("hops") && j["hops"].is_number_integer()) {
            r.hops = j["hops"].get<int>();
        }
        // Bound the fan-out so a reply stays inside one datagram.
        r.hops = std::clamp(r.hops, 1, 4);
        return r;
    }
    if (q == "search") {
        if (!j.contains("text") || !j["text"].is_string()
            || j["text"].get<std::string>().empty()) {
            return invalid(req, "search requires non-empty \"text\"");
        }
        r.kind = QueryRequest::Kind::Search;
        r.text = j["text"].get<std::string>();
        return r;
    }
    if (q == "node") {
        if (!j.contains("id") || !j["id"].is_number_integer()) {
            return invalid(req, "node requires integer \"id\"");
        }
        r.kind = QueryRequest::Kind::Node;
        r.id   = j["id"].get<long long>();
        return r;
    }
    return invalid(req, "unknown query \"" + q + "\"");
}

std::string serialize_response(const QueryResponse& r) {
    nlohmann::json j;
    j["req"] = r.req;
    if (!r.error.empty()) {
        j["error"] = r.error;
        return j.dump();
    }
    nlohmann::json nodes = nlohmann::json::array();
    for (const QueryNode& n : r.nodes) {
        nodes.push_back({
            {"id",      n.id},
            {"title",   n.title},
            {"content", n.content},
            {"tier",    n.tier},
            {"tags",    n.tags},
            {"x",       n.x},
            {"y",       n.y},
            {"z",       n.z},
            {"score",   n.score},
        });
    }
    nlohmann::json edges = nlohmann::json::array();
    for (const QueryEdge& e : r.edges) {
        edges.push_back({
            {"source",    e.source},
            {"target",    e.target},
            {"kind",      e.kind},
            {"certainty", e.certainty},
        });
    }
    j["nodes"] = std::move(nodes);
    j["edges"] = std::move(edges);
    return j.dump();
}

std::string truncate_utf8(std::string_view s, std::size_t max_bytes) {
    if (s.size() <= max_bytes) return std::string(s);

    static constexpr char        kEllipsis[]  = "\xE2\x80\xA6";  // U+2026 "…"
    static constexpr std::size_t kEllipsisLen = 3;

    // Leave room for the marker when we can; otherwise just hard-cut.
    std::size_t cut = (max_bytes >= kEllipsisLen) ? max_bytes - kEllipsisLen : max_bytes;
    // Back off out of the middle of a multibyte codepoint: continuation bytes
    // match 10xxxxxx, so move down until `cut` lands on a lead/ASCII byte.
    while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80) --cut;

    std::string out(s.substr(0, cut));
    if (max_bytes >= kEllipsisLen) out += kEllipsis;
    return out;
}

}  // namespace zg::telemetry
