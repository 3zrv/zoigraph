#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "graph/timeline.h"

#include <cmath>

using zg::graph::compute_timeline_positions;

TEST_CASE("timeline: empty input -> empty output") {
    CHECK(compute_timeline_positions({}).empty());
}

TEST_CASE("timeline: single node lands at origin") {
    const auto out = compute_timeline_positions({100.0});
    REQUIRE(out.size() == 1);
    CHECK(out[0].x == doctest::Approx(0.0f));
    CHECK(out[0].y == doctest::Approx(0.0f));
    CHECK(out[0].z == doctest::Approx(0.0f));
}

TEST_CASE("timeline: nodes with distinct timestamps spread across the x span") {
    const auto out = compute_timeline_positions({100.0, 200.0, 300.0}, 60.0f, 0.0f);
    REQUIRE(out.size() == 3);
    // First node at the earliest time -> -x_span.
    CHECK(out[0].x == doctest::Approx(-60.0f));
    // Last node at the latest time -> +x_span.
    CHECK(out[2].x == doctest::Approx(60.0f));
    // Middle node at the midpoint -> 0.
    CHECK(out[1].x == doctest::Approx(0.0f));
}

TEST_CASE("timeline: jitter on y stays within the configured range") {
    const auto out = compute_timeline_positions(
        {100.0, 200.0, 300.0, 400.0, 500.0}, 60.0f, /*y_jitter=*/10.0f);
    REQUIRE(out.size() == 5);
    for (const auto& p : out) {
        CHECK(p.y >= -10.0f);
        CHECK(p.y <=  10.0f);
        CHECK(p.z == doctest::Approx(0.0f));
    }
}

TEST_CASE("timeline: y jitter is deterministic per index") {
    const auto a = compute_timeline_positions({100.0, 200.0, 300.0}, 60.0f, 10.0f);
    const auto b = compute_timeline_positions({100.0, 200.0, 300.0}, 60.0f, 10.0f);
    REQUIRE(a.size() == b.size());
    for (std::size_t i = 0; i < a.size(); ++i) {
        CHECK(a[i].y == doctest::Approx(b[i].y));
    }
}

TEST_CASE("timeline: nodes with first_seen == 0 are treated as earliest") {
    // First node has unknown timestamp; should land at the same x as the
    // earliest known one (t=200).
    const auto out = compute_timeline_positions({0.0, 200.0, 300.0}, 60.0f, 0.0f);
    REQUIRE(out.size() == 3);
    CHECK(out[0].x == doctest::Approx(-60.0f));  // unknown -> earliest
    CHECK(out[1].x == doctest::Approx(-60.0f));  // 200 is earliest known
    CHECK(out[2].x == doctest::Approx(60.0f));   // 300 is latest
}

TEST_CASE("timeline: all-equal timestamps fall back to even index spacing") {
    const auto out = compute_timeline_positions({100.0, 100.0, 100.0, 100.0}, 60.0f, 0.0f);
    REQUIRE(out.size() == 4);
    CHECK(out[0].x == doctest::Approx(-60.0f));
    CHECK(out[3].x == doctest::Approx(60.0f));
    // Monotonically increasing x.
    CHECK(out[0].x < out[1].x);
    CHECK(out[1].x < out[2].x);
    CHECK(out[2].x < out[3].x);
}

TEST_CASE("timeline: all-zero timestamps also fall back to even index spacing") {
    const auto out = compute_timeline_positions({0.0, 0.0, 0.0}, 60.0f, 0.0f);
    REQUIRE(out.size() == 3);
    CHECK(out[0].x == doctest::Approx(-60.0f));
    CHECK(out[1].x == doctest::Approx(0.0f));
    CHECK(out[2].x == doctest::Approx(60.0f));
}
