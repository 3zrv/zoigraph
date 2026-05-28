#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "app/ask.h"

using zg::app::last_nonblank_line;
using zg::app::tcp_probe_localhost;

TEST_CASE("last_nonblank_line: empty input returns empty") {
    CHECK(last_nonblank_line("").empty());
}

TEST_CASE("last_nonblank_line: single line without newline returns itself") {
    CHECK(last_nonblank_line("hello") == "hello");
}

TEST_CASE("last_nonblank_line: trailing newline is stripped") {
    CHECK(last_nonblank_line("hello\n") == "hello");
}

TEST_CASE("last_nonblank_line: trailing CRLF is stripped") {
    CHECK(last_nonblank_line("hello\r\n") == "hello");
}

TEST_CASE("last_nonblank_line: multiple lines returns the last non-blank one") {
    CHECK(last_nonblank_line("first\nsecond\nthird") == "third");
}

TEST_CASE("last_nonblank_line: trailing blank lines are skipped") {
    CHECK(last_nonblank_line("real\n\n\n") == "real");
}

TEST_CASE("last_nonblank_line: only-blank input returns empty") {
    CHECK(last_nonblank_line("\n\n\n").empty());
    CHECK(last_nonblank_line("\r\n\r\n").empty());
}

TEST_CASE("last_nonblank_line: surrounded by noise still picks the last line") {
    const std::string blob =
        "=== raw response ===\n"
        "{junk}\n"
        "=== parse ===\n"
        "schema invalid -- not sending UDP packet\n";
    CHECK(last_nonblank_line(blob) == "schema invalid -- not sending UDP packet");
}

TEST_CASE("tcp_probe_localhost: refused port returns false") {
    // Port 1 is reserved and never listened on by user processes; connect
    // returns ECONNREFUSED instantly. This is the "Ollama not running"
    // path the inspector relies on for fast error feedback.
    CHECK_FALSE(tcp_probe_localhost(1));
}

TEST_CASE("tcp_probe_localhost: another almost-certainly-closed port returns false") {
    // Belt-and-braces: a high ephemeral-range port nobody binds by default.
    // If this ever returns true on a CI box, we've got bigger problems
    // than this test.
    CHECK_FALSE(tcp_probe_localhost(57321));
}
