#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "app/query_responder.h"

#include <string>
#include <vector>

using zg::app::answer_query;
using zg::graph::Edge;
using zg::persistence::StoredNode;
using zg::telemetry::QueryRequest;
using Kind = zg::telemetry::QueryRequest::Kind;

namespace {

const std::string kTok = "sekret";

StoredNode mk(long long id, std::string title, std::string content,
              bool deleted = false) {
    StoredNode n{};
    n.id       = id;
    n.position = {static_cast<float>(id), 0, 0};  // distinct x so passthrough is checkable
    n.title    = std::move(title);
    n.content  = std::move(content);
    n.tier     = "confirmed";
    n.deleted  = deleted;
    return n;
}

QueryRequest req(Kind k, long long id, std::string token = kTok) {
    QueryRequest r;
    r.kind  = k;
    r.req   = 1;
    r.id    = id;
    r.token = std::move(token);
    return r;
}

// A search stub that returns a fixed id list regardless of query text.
auto stub(std::vector<long long> ids) {
    return [ids](const std::string&) { return ids; };
}

const auto no_search = stub({});

}  // namespace

TEST_CASE("responder: a wrong or empty token is dropped (no reply)") {
    std::vector<StoredNode> nodes = {mk(0, "a", "")};
    std::vector<Edge> edges;

    CHECK_FALSE(answer_query(req(Kind::Node, 0, "wrong"), kTok, nodes, edges, no_search)
                    .has_value());
    // Empty *configured* token closes the channel entirely.
    CHECK_FALSE(answer_query(req(Kind::Node, 0, ""), "", nodes, edges, no_search)
                    .has_value());
}

TEST_CASE("responder: a node query returns the single live node") {
    std::vector<StoredNode> nodes = {mk(0, "alpha", "body"), mk(1, "beta", "")};
    std::vector<Edge> edges;

    auto r = answer_query(req(Kind::Node, 1), kTok, nodes, edges, no_search);
    REQUIRE(r.has_value());
    CHECK(r->error.empty());
    REQUIRE(r->nodes.size() == 1);
    CHECK(r->nodes[0].id == 1);
    CHECK(r->nodes[0].title == "beta");
    CHECK(r->nodes[0].x == doctest::Approx(1.0f));  // position carried through
}

TEST_CASE("responder: a tombstoned or out-of-range node is an error, not a node") {
    std::vector<StoredNode> nodes = {mk(0, "a", ""), mk(1, "ghost", "", /*deleted=*/true)};
    std::vector<Edge> edges;

    auto dead = answer_query(req(Kind::Node, 1), kTok, nodes, edges, no_search);
    REQUIRE(dead.has_value());
    CHECK(dead->nodes.empty());
    CHECK(dead->error == "no such node");

    auto oob = answer_query(req(Kind::Node, 99), kTok, nodes, edges, no_search);
    REQUIRE(oob.has_value());
    CHECK(oob->error == "no such node");
}

TEST_CASE("responder: search drops tombstoned and out-of-range hits") {
    std::vector<StoredNode> nodes = {
        mk(0, "a", ""), mk(1, "b", "", /*deleted=*/true), mk(2, "c", "")};
    std::vector<Edge> edges;

    // stub yields 0 (live), 1 (dead), 2 (live), 9 (oob).
    QueryRequest s;
    s.kind = Kind::Search; s.req = 1; s.text = "x"; s.token = kTok;
    auto r = answer_query(s, kTok, nodes, edges, stub({0, 1, 2, 9}));
    REQUIRE(r.has_value());
    REQUIRE(r->nodes.size() == 2);
    CHECK(r->nodes[0].id == 0);
    CHECK(r->nodes[1].id == 2);
}

TEST_CASE("responder: neighborhood returns anchor-first, relevance-ranked, with edges") {
    // path 0-1-2-3
    std::vector<StoredNode> nodes = {
        mk(0, "n0", ""), mk(1, "n1", ""), mk(2, "n2", ""), mk(3, "n3", "")};
    std::vector<Edge> edges = {{0, 1}, {1, 2}, {2, 3}};

    QueryRequest n;
    n.kind = Kind::Neighborhood; n.req = 1; n.id = 0; n.hops = 1; n.token = kTok;
    auto r = answer_query(n, kTok, nodes, edges, no_search);
    REQUIRE(r.has_value());
    REQUIRE(r->nodes.size() == 4);
    CHECK(r->nodes[0].id == 0);          // anchor first
    CHECK(r->nodes[1].id == 1);          // nearest neighbour ranks above farther ones
    CHECK(r->nodes[1].score > r->nodes[2].score);
    CHECK(r->edges.size() == 3);         // all three path edges, both ends in-set
}

TEST_CASE("responder: a dead node blocks the neighborhood beyond it") {
    // path 0-1-2-3 with node 2 tombstoned -> 3 unreachable from 0.
    std::vector<StoredNode> nodes = {
        mk(0, "n0", ""), mk(1, "n1", ""),
        mk(2, "n2", "", /*deleted=*/true), mk(3, "n3", "")};
    std::vector<Edge> edges = {{0, 1}, {1, 2}, {2, 3}};

    QueryRequest n;
    n.kind = Kind::Neighborhood; n.req = 1; n.id = 0; n.hops = 4; n.token = kTok;
    auto r = answer_query(n, kTok, nodes, edges, no_search);
    REQUIRE(r.has_value());
    REQUIRE(r->nodes.size() == 2);       // anchor 0 + live reachable 1 only
    CHECK(r->nodes[0].id == 0);
    CHECK(r->nodes[1].id == 1);
    REQUIRE(r->edges.size() == 1);       // only 0-1; edges touching dead node 2 dropped
    CHECK(r->edges[0].source == 0);
    CHECK(r->edges[0].target == 1);
}

TEST_CASE("responder: a tombstoned anchor is an error") {
    std::vector<StoredNode> nodes = {mk(0, "ghost", "", /*deleted=*/true), mk(1, "a", "")};
    std::vector<Edge> edges = {{0, 1}};
    auto r = answer_query(req(Kind::Neighborhood, 0), kTok, nodes, edges, no_search);
    REQUIRE(r.has_value());
    CHECK(r->error == "no such node");
}

TEST_CASE("responder: an authorized-but-invalid request echoes the parse error") {
    std::vector<StoredNode> nodes = {mk(0, "a", "")};
    std::vector<Edge> edges;
    QueryRequest bad;
    bad.kind = Kind::Invalid; bad.req = 77; bad.token = kTok; bad.error = "unknown query \"x\"";
    auto r = answer_query(bad, kTok, nodes, edges, no_search);
    REQUIRE(r.has_value());
    CHECK(r->req == 77);
    CHECK(r->error == "unknown query \"x\"");
}

TEST_CASE("responder: node content is truncated to the cap") {
    std::vector<StoredNode> nodes = {mk(0, "a", std::string(2000, 'z'))};
    std::vector<Edge> edges;
    auto r = answer_query(req(Kind::Node, 0), kTok, nodes, edges, no_search, /*cap=*/50);
    REQUIRE(r.has_value());
    REQUIRE(r->nodes.size() == 1);
    CHECK(r->nodes[0].content.size() <= 50);
}
