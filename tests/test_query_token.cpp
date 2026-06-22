#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "app/query_token.h"

#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

using zg::app::generate_session_token;
using zg::app::write_token_file;

TEST_CASE("token: 32 lowercase hex chars") {
    const std::string t = generate_session_token();
    CHECK(t.size() == 32);
    for (char c : t) {
        const bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        CHECK(hex);
    }
}

TEST_CASE("token: successive tokens differ") {
    // Not a randomness proof, just a smoke check that we're not emitting a
    // constant. Collision probability across two 128-bit draws is negligible.
    CHECK(generate_session_token() != generate_session_token());
}

TEST_CASE("token: write_token_file round-trips the exact bytes, no newline") {
    namespace fs = std::filesystem;
    const fs::path p = fs::temp_directory_path() / "zoigraph_token_test.tok";
    std::remove(p.string().c_str());

    const std::string tok = generate_session_token();
    REQUIRE(write_token_file(p, tok));

    std::ifstream in(p, std::ios::binary);
    REQUIRE(in.good());
    std::string back((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
    CHECK(back == tok);  // exact, no trailing '\n'

    std::remove(p.string().c_str());
}
