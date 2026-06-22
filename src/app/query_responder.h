#pragma once

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "graph/types.h"
#include "persistence/db.h"  // StoredNode
#include "telemetry/query_protocol.h"

namespace zg::app {

// Answer one parsed query against the live graph (render-thread owned). Pure
// except for the injected `search` callback — the FTS index lives in the DB —
// which keeps the whole thing unit-testable with a stub.
//
// Auth: an empty configured token, or any mismatch, returns std::nullopt — the
// request is dropped with no reply at all. The read channel exposes node
// content, so an unauthenticated caller gets nothing, not even an error that
// would confirm the channel exists. An *authenticated* but malformed request
// instead gets an error reply, so a legitimate client can debug.
//
// Relies on the T4.2 invariant (node id == vector index): a node id indexes
// `nodes` directly, and tombstoned (deleted) nodes are never returned and
// never routed through. `search` yields candidate ids (e.g. Database::search);
// the responder drops tombstones and caps the count. `content_cap` bounds each
// node's body so a reply fits a single datagram.
std::optional<telemetry::QueryResponse> answer_query(
    const telemetry::QueryRequest& req,
    const std::string& token,
    const std::vector<persistence::StoredNode>& nodes,
    const std::vector<graph::Edge>& edges,
    const std::function<std::vector<long long>(const std::string&)>& search,
    std::size_t content_cap = 500);

}  // namespace zg::app
