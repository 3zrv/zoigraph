#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "telemetry/query_protocol.h"

#include <string>

#include <nlohmann/json.hpp>

using zg::telemetry::parse_query;
using zg::telemetry::serialize_response;
using zg::telemetry::truncate_utf8;
using zg::telemetry::QueryRequest;
using zg::telemetry::QueryResponse;
using zg::telemetry::QueryNode;
using zg::telemetry::QueryEdge;
using Kind = QueryRequest::Kind;

TEST_CASE("query: a well-formed neighborhood request parses with all fields") {
    auto r = parse_query(R"({"q":"neighborhood","req":7,"id":3,"hops":2,"token":"sekret"})");
    CHECK(r.kind == Kind::Neighborhood);
    CHECK(r.req == 7);
    CHECK(r.id == 3);
    CHECK(r.hops == 2);
    CHECK(r.token == "sekret");
    CHECK(r.error.empty());
}

TEST_CASE("query: search and node requests parse") {
    auto s = parse_query(R"({"q":"search","req":8,"text":"foo bar","token":"t"})");
    CHECK(s.kind == Kind::Search);
    CHECK(s.text == "foo bar");
    CHECK(s.req == 8);

    auto n = parse_query(R"({"q":"node","req":9,"id":5,"token":"t"})");
    CHECK(n.kind == Kind::Node);
    CHECK(n.id == 5);
}

TEST_CASE("query: hops defaults to 1 and is clamped to [1,4]") {
    CHECK(parse_query(R"({"q":"neighborhood","id":1})").hops == 1);   // default
    CHECK(parse_query(R"({"q":"neighborhood","id":1,"hops":0})").hops == 1);   // clamp low
    CHECK(parse_query(R"({"q":"neighborhood","id":1,"hops":99})").hops == 4);  // clamp high
    CHECK(parse_query(R"({"q":"neighborhood","id":1,"hops":-5})").hops == 1);
}

TEST_CASE("query: req is optional and defaults to -1, still echoed on error") {
    auto r = parse_query(R"({"q":"node","id":2})");
    CHECK(r.kind == Kind::Node);
    CHECK(r.req == -1);

    // req present but the query is malformed: the error still carries req back.
    auto bad = parse_query(R"({"q":"bogus","req":42})");
    CHECK(bad.kind == Kind::Invalid);
    CHECK(bad.req == 42);
    CHECK_FALSE(bad.error.empty());
}

TEST_CASE("query: malformed inputs all land on Invalid with a reason") {
    CHECK(parse_query("not json at all").kind == Kind::Invalid);
    CHECK(parse_query("[1,2,3]").kind == Kind::Invalid);              // not an object
    CHECK(parse_query(R"({"req":1})").kind == Kind::Invalid);         // missing q
    CHECK(parse_query(R"({"q":"search"})").kind == Kind::Invalid);    // search w/o text
    CHECK(parse_query(R"({"q":"search","text":""})").kind == Kind::Invalid);  // empty text
    CHECK(parse_query(R"({"q":"neighborhood"})").kind == Kind::Invalid);      // no id
    CHECK(parse_query(R"({"q":"node","id":"x"})").kind == Kind::Invalid);     // id wrong type
    for (auto* p : {"not json at all", R"({"q":"neighborhood"})"}) {
        CHECK_FALSE(parse_query(p).error.empty());
    }
}

TEST_CASE("query: oversized payloads are rejected before parsing") {
    std::string big = R"({"q":"search","text":")" + std::string(2000, 'x') + R"("})";
    auto r = parse_query(big, /*max_bytes=*/1024);
    CHECK(r.kind == Kind::Invalid);
    CHECK(r.error == "payload too large");
}

TEST_CASE("query: an error response is {req, error} with no node arrays") {
    QueryResponse r;
    r.req   = 5;
    r.error = "no such node";
    auto j = nlohmann::json::parse(serialize_response(r));
    CHECK(j["req"] == 5);
    CHECK(j["error"] == "no such node");
    CHECK_FALSE(j.contains("nodes"));
}

TEST_CASE("query: a full response round-trips through serialize + JSON parse") {
    QueryResponse r;
    r.req = 11;
    r.nodes.push_back(QueryNode{1, "alpha", "body a", "confirmed", {"asset"},
                                1.5f, 2.5f, 3.5f, 0.42f});
    r.nodes.push_back(QueryNode{2, "beta", "body b", "phantom", {},
                                0.0f, 0.0f, 0.0f, 0.1f});
    r.edges.push_back(QueryEdge{1, 2, "knows", "suspected"});

    auto j = nlohmann::json::parse(serialize_response(r));
    CHECK(j["req"] == 11);
    REQUIRE(j["nodes"].size() == 2);
    CHECK(j["nodes"][0]["id"] == 1);
    CHECK(j["nodes"][0]["title"] == "alpha");
    CHECK(j["nodes"][0]["tier"] == "confirmed");
    CHECK(j["nodes"][0]["tags"][0] == "asset");
    CHECK(j["nodes"][0]["x"].get<float>() == doctest::Approx(1.5f));
    CHECK(j["nodes"][0]["z"].get<float>() == doctest::Approx(3.5f));
    CHECK(j["nodes"][0]["score"].get<float>() == doctest::Approx(0.42f));
    REQUIRE(j["edges"].size() == 1);
    CHECK(j["edges"][0]["source"] == 1);
    CHECK(j["edges"][0]["kind"] == "knows");
    CHECK(j["edges"][0]["certainty"] == "suspected");
}

TEST_CASE("query: an empty result is still well-formed (empty arrays)") {
    QueryResponse r;
    r.req = 3;
    auto j = nlohmann::json::parse(serialize_response(r));
    CHECK(j["req"] == 3);
    CHECK(j["nodes"].is_array());
    CHECK(j["nodes"].empty());
    CHECK(j["edges"].empty());
}

TEST_CASE("truncate_utf8: short strings pass through untouched") {
    CHECK(truncate_utf8("hello", 100) == "hello");
    CHECK(truncate_utf8("", 0) == "");
    CHECK(truncate_utf8("exact", 5) == "exact");  // == boundary, no cut
}

TEST_CASE("truncate_utf8: ASCII is cut with an ellipsis, within the byte budget") {
    auto out = truncate_utf8("abcdefghij", 5);
    CHECK(out == std::string("ab") + "\xE2\x80\xA6");  // 2 + 3 == 5 bytes
    CHECK(out.size() <= 5);
}

TEST_CASE("truncate_utf8: never splits a multibyte codepoint") {
    // "aé..." where é is C3 A9 (2 bytes). A naive cut at byte 2 would slice é;
    // the function must back off to keep it whole.
    std::string s = "a\xC3\xA9" "bcdefgh";
    auto out = truncate_utf8(s, 5);          // cut target = 5-3 = byte 2 (the A9)
    CHECK(out == std::string("a") + "\xE2\x80\xA6");  // backed off to drop é entirely
    // No dangling lead byte left at the end of the kept prefix.
    CHECK(out.find('\xC3') == std::string::npos);
}
