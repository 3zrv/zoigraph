#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace zg::telemetry {

// ---- Requests -------------------------------------------------------------

// One read query parsed from a loopback datagram on the query channel
// (sibling to the phantom-inject channel, but request/response). Three kinds;
// anything malformed — bad JSON, unknown/missing "q", wrong-typed fields,
// oversized payload — parses to Kind::Invalid with `error` set, so the
// responder can echo a diagnostic instead of going silent (UDP gives no other
// failure signal).
struct QueryRequest {
    enum class Kind { Invalid, Neighborhood, Search, Node };
    Kind        kind = Kind::Invalid;
    long long   req  = -1;    // client correlation id, echoed back verbatim
    std::string token;        // session auth token; verified by the responder, not here
    long long   id   = -1;    // node id (Neighborhood, Node)
    int         hops = 1;     // Neighborhood fan-out bound, clamped to [1, 4]
    std::string text;         // Search terms
    std::string error;        // human-readable reason when kind == Invalid
};

// Parse one datagram. Never throws. Payloads longer than `max_bytes` parse to
// Invalid (the socket layer drops oversized datagrams too — this is
// defence-in-depth for non-socket callers and tests). The token is surfaced
// but NOT verified here: verification is the responder's job because it holds
// the per-session secret, and keeping this function pure keeps it testable.
QueryRequest parse_query(std::string_view payload, std::size_t max_bytes = 1024);

// ---- Responses ------------------------------------------------------------

// A node as the channel exposes it — enough for LLM prompt assembly, never the
// whole row. `content` is pre-truncated by the responder (see truncate_utf8).
struct QueryNode {
    long long                id;
    std::string              title;
    std::string              content;
    std::string              tier;
    std::vector<std::string> tags;
    float                    x = 0.0f;      // position — lets the bridge place a phantom
    float                    y = 0.0f;      // near the centroid of its chosen referents
    float                    z = 0.0f;
    float                    score = 0.0f;  // PPR relevance for Neighborhood; 0 otherwise
};

// One edge in a Neighborhood response.
struct QueryEdge {
    long long   source;
    long long   target;
    std::string kind;
    std::string certainty;
};

// The responder fills this from its own graph data; serialize_response turns
// it into the reply datagram. A non-empty `error` short-circuits to
// {"req":N,"error":"..."} with no nodes/edges.
struct QueryResponse {
    long long              req = -1;
    std::vector<QueryNode> nodes;
    std::vector<QueryEdge> edges;
    std::string            error;
};

// Serialize to one JSON datagram:
//   {"req":N,"nodes":[{id,title,content,tier,tags,score}],
//            "edges":[{source,target,kind,certainty}]}
// or {"req":N,"error":"..."} when error is set.
std::string serialize_response(const QueryResponse& r);

// UTF-8-safe truncation to at most `max_bytes` bytes: never splits a multibyte
// codepoint, and appends a single "…" (U+2026, 3 bytes) marker when it cuts.
// Drives the per-node content cap so a neighbourhood reply fits one datagram.
std::string truncate_utf8(std::string_view s, std::size_t max_bytes);

}  // namespace zg::telemetry
