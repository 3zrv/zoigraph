#include "app/query_responder.h"

#include <algorithm>

#include "graph/ppr.h"

namespace zg::app {

namespace {

using telemetry::QueryEdge;
using telemetry::QueryNode;
using telemetry::QueryRequest;
using telemetry::QueryResponse;
using persistence::StoredNode;

bool in_range(long long id, std::size_t n) {
    return id >= 0 && static_cast<std::size_t>(id) < n;
}

bool alive(const std::vector<StoredNode>& nodes, long long id) {
    return in_range(id, nodes.size())
        && !nodes[static_cast<std::size_t>(id)].deleted;
}

QueryNode make_node(const StoredNode& n, float score, std::size_t content_cap) {
    QueryNode qn;
    qn.id      = n.id;
    qn.title   = n.title;
    qn.content = telemetry::truncate_utf8(n.content, content_cap);
    qn.tier    = n.tier;
    qn.tags    = n.tags;
    qn.x       = n.position.x;
    qn.y       = n.position.y;
    qn.z       = n.position.z;
    qn.score   = score;
    return qn;
}

std::vector<char> alive_mask(const std::vector<StoredNode>& nodes) {
    std::vector<char> mask(nodes.size());
    for (std::size_t i = 0; i < nodes.size(); ++i) mask[i] = !nodes[i].deleted;
    return mask;
}

}  // namespace

std::optional<QueryResponse> answer_query(
    const QueryRequest& req,
    const std::string& token,
    const std::vector<StoredNode>& nodes,
    const std::vector<graph::Edge>& edges,
    const std::function<std::vector<long long>(const std::string&)>& search,
    std::size_t content_cap) {
    // Auth gate: an empty configured token closes the channel, and any
    // mismatch is a silent drop (no reply confirms the channel to a stranger).
    if (token.empty() || req.token != token) return std::nullopt;

    QueryResponse out;
    out.req = req.req;

    switch (req.kind) {
        case QueryRequest::Kind::Invalid:
            out.error = req.error.empty() ? "invalid query" : req.error;
            return out;

        case QueryRequest::Kind::Node: {
            if (!alive(nodes, req.id)) { out.error = "no such node"; return out; }
            out.nodes.push_back(
                make_node(nodes[static_cast<std::size_t>(req.id)], 0.0f, content_cap));
            return out;
        }

        case QueryRequest::Kind::Search: {
            constexpr std::size_t kMaxSearch = 16;
            for (long long id : search(req.text)) {
                if (out.nodes.size() >= kMaxSearch) break;
                if (!alive(nodes, id)) continue;  // tombstoned / out of range
                out.nodes.push_back(
                    make_node(nodes[static_cast<std::size_t>(id)], 0.0f, content_cap));
            }
            return out;
        }

        case QueryRequest::Kind::Neighborhood: {
            if (!alive(nodes, req.id)) { out.error = "no such node"; return out; }
            const std::size_t anchor = static_cast<std::size_t>(req.id);
            const std::vector<char> mask = alive_mask(nodes);
            const std::vector<float> rank =
                graph::personalized_pagerank(nodes.size(), edges, anchor, mask);

            // hops scales how much context to return; PPR ranks it by relevance.
            const std::size_t budget =
                static_cast<std::size_t>(std::clamp(req.hops * 6, 6, 24));

            // Top (budget - 1) relatives by score: anchor, dead, and
            // unreachable (zero-score) nodes excluded; ties broken by index.
            std::vector<std::size_t> rel;
            for (std::size_t i = 0; i < nodes.size(); ++i) {
                if (i == anchor || !mask[i] || rank[i] <= 0.0f) continue;
                rel.push_back(i);
            }
            std::sort(rel.begin(), rel.end(), [&rank](std::size_t a, std::size_t b) {
                if (rank[a] != rank[b]) return rank[a] > rank[b];
                return a < b;
            });
            if (rel.size() > budget - 1) rel.resize(budget - 1);

            // Anchor first, then relatives, each carrying its PPR score.
            out.nodes.push_back(make_node(nodes[anchor], rank[anchor], content_cap));
            for (std::size_t i : rel) {
                out.nodes.push_back(make_node(nodes[i], rank[i], content_cap));
            }

            // Edges with BOTH endpoints in the returned, alive set — so the
            // model sees structure, not just a node list.
            std::vector<char> included(nodes.size(), 0);
            included[anchor] = 1;
            for (std::size_t i : rel) included[i] = 1;
            for (const graph::Edge& e : edges) {
                if (e.source >= nodes.size() || e.target >= nodes.size()) continue;
                if (!included[e.source] || !included[e.target]) continue;
                if (!mask[e.source] || !mask[e.target]) continue;
                out.edges.push_back(QueryEdge{
                    static_cast<long long>(e.source),
                    static_cast<long long>(e.target),
                    e.kind, e.certainty});
            }
            return out;
        }
    }
    out.error = "unhandled query kind";  // unreachable; keeps the compiler happy
    return out;
}

}  // namespace zg::app
