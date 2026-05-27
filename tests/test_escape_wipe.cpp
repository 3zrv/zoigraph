#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "input/escape_wipe.h"

using zg::input::EscapeWipe;

TEST_CASE("escape_wipe: a single press never triggers") {
    EscapeWipe w;
    CHECK_FALSE(w.record(100.0, 1.0));
}

TEST_CASE("escape_wipe: two presses within the window don't trigger") {
    EscapeWipe w;
    CHECK_FALSE(w.record(100.0, 1.0));
    CHECK_FALSE(w.record(100.3, 1.0));
}

TEST_CASE("escape_wipe: three presses within the window trigger") {
    EscapeWipe w;
    CHECK_FALSE(w.record(100.0, 1.0));
    CHECK_FALSE(w.record(100.3, 1.0));
    CHECK     (w.record(100.6, 1.0));
}

TEST_CASE("escape_wipe: three presses spread beyond the window do not trigger") {
    EscapeWipe w;
    CHECK_FALSE(w.record(100.0, 1.0));
    CHECK_FALSE(w.record(102.0, 1.0));
    CHECK_FALSE(w.record(104.0, 1.0));
}

TEST_CASE("escape_wipe: after one out-of-window press, two fast presses don't fire") {
    EscapeWipe w;
    w.record(100.0, 1.0);
    w.record(200.0, 1.0);  // far apart
    CHECK_FALSE(w.record(200.1, 1.0));  // only 2 of the 3 stamps are recent
}

TEST_CASE("escape_wipe: four fast presses keep firing on each new press") {
    EscapeWipe w;
    w.record(100.0, 1.0);
    w.record(100.2, 1.0);
    CHECK(w.record(100.4, 1.0));  // first triple
    CHECK(w.record(100.6, 1.0));  // second triple — sliding window
}

TEST_CASE("escape_wipe: triple after a long pause requires three new presses") {
    EscapeWipe w;
    w.record(100.0, 1.0);
    w.record(100.5, 1.0);
    CHECK(w.record(100.9, 1.0));  // triggers
    // Long pause — older stamps drift out of window. A single new press
    // should not re-trigger because only 1 of the 3 stamps is current.
    CHECK_FALSE(w.record(300.0, 1.0));
    CHECK_FALSE(w.record(300.2, 1.0));
    CHECK     (w.record(300.4, 1.0));
}

TEST_CASE("escape_wipe: zero window only triggers when all stamps are equal") {
    EscapeWipe w;
    w.record(100.0, 0.0);
    w.record(100.0, 0.0);
    CHECK(w.record(100.0, 0.0));  // all three identical -> max - min = 0

    EscapeWipe w2;
    w2.record(100.0, 0.0);
    w2.record(100.0, 0.0);
    CHECK_FALSE(w2.record(100.000001, 0.0));  // tiny drift defeats zero window
}

TEST_CASE("escape_wipe: configurable window") {
    EscapeWipe w;
    w.record(100.0, 0.2);
    w.record(100.1, 0.2);
    CHECK     (w.record(100.15, 0.2));  // within 0.2s window

    EscapeWipe w2;
    w2.record(100.0, 0.2);
    w2.record(100.1, 0.2);
    CHECK_FALSE(w2.record(100.5, 0.2));  // outside 0.2s window
}
