#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "graph/wikilinks.h"

using zg::graph::extract_wikilinks;

TEST_CASE("wikilinks: no links in plain text returns empty") {
    CHECK(extract_wikilinks("").empty());
    CHECK(extract_wikilinks("plain markdown with no links at all").empty());
}

TEST_CASE("wikilinks: a single [[link]] is extracted verbatim") {
    const auto out = extract_wikilinks("see [[other-node]] for context");
    REQUIRE(out.size() == 1);
    CHECK(out[0] == "other-node");
}

TEST_CASE("wikilinks: multiple links in order of appearance") {
    const auto out = extract_wikilinks(
        "[[alpha]] then [[beta]] later [[gamma]] end");
    REQUIRE(out.size() == 3);
    CHECK(out[0] == "alpha");
    CHECK(out[1] == "beta");
    CHECK(out[2] == "gamma");
}

TEST_CASE("wikilinks: duplicates are preserved (caller dedupes if needed)") {
    const auto out = extract_wikilinks("[[x]] said [[x]] again");
    REQUIRE(out.size() == 2);
    CHECK(out[0] == "x");
    CHECK(out[1] == "x");
}

TEST_CASE("wikilinks: titles with spaces, punctuation, and unicode") {
    const auto out = extract_wikilinks(
        "[[scan host 1.2.3.4]] and [[Café αβγ]]");
    REQUIRE(out.size() == 2);
    CHECK(out[0] == "scan host 1.2.3.4");
    CHECK(out[1] == "Café αβγ");
}

TEST_CASE("wikilinks: unterminated [[ is skipped without crashing") {
    // No closing ]] anywhere — the half-open bracket is dropped and the
    // rest of the content is scanned.
    const auto out = extract_wikilinks("text [[never closed and that's all");
    CHECK(out.empty());
}

TEST_CASE("wikilinks: a fully-formed link after an unterminated one is found") {
    const auto out = extract_wikilinks("[[never closed but then [[real]]");
    REQUIRE(out.size() == 1);
    CHECK(out[0] == "real");
}

TEST_CASE("wikilinks: single brackets are ignored") {
    const auto out = extract_wikilinks("[not a link] but [[is]]");
    REQUIRE(out.size() == 1);
    CHECK(out[0] == "is");
}

TEST_CASE("wikilinks: empty target [[]] yields an empty string entry") {
    // Lexically valid; the resolver should drop empties, but the extractor
    // doesn't pre-filter.
    const auto out = extract_wikilinks("hmm [[]] huh");
    REQUIRE(out.size() == 1);
    CHECK(out[0].empty());
}

TEST_CASE("wikilinks: single brackets inside an open [[ are kept in the target") {
    // Once we're inside a [[, a plain "[" or "]" character is part of the
    // target until we see "]]" (or another "[[" which restarts the scan).
    const auto out = extract_wikilinks("[[some [bracketed] text]] rest");
    REQUIRE(out.size() == 1);
    CHECK(out[0] == "some [bracketed] text");
}

TEST_CASE("wikilinks: many links in a long content blob") {
    std::string content;
    for (int i = 0; i < 200; ++i) {
        content += "filler text ";
        content += "[[node-" + std::to_string(i) + "]] ";
    }
    const auto out = extract_wikilinks(content);
    REQUIRE(out.size() == 200);
    CHECK(out[0]   == "node-0");
    CHECK(out[199] == "node-199");
}

TEST_CASE("wikilinks: adjacent links with no separator both extract") {
    const auto out = extract_wikilinks("[[a]][[b]]");
    REQUIRE(out.size() == 2);
    CHECK(out[0] == "a");
    CHECK(out[1] == "b");
}
