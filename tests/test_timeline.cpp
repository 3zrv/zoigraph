#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "graph/timeline.h"

#include <algorithm>
#include <cmath>
#include <set>
#include <vector>

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

// ---- Column stacking when nodes cluster at the same timestamp -------------

TEST_CASE("timeline: 5 nodes at the same late timestamp stack at distinct ys") {
    // 1 node at t=100, 5 nodes at t=500. The 5 at the latest end should
    // share the same x (rightmost) but each get a distinct y so they don't
    // pile on top of each other.
    const auto out = compute_timeline_positions(
        {100.0, 500.0, 500.0, 500.0, 500.0, 500.0},
        /*x_span=*/60.0f, /*y_jitter=*/10.0f, /*column_spacing=*/3.0f);
    REQUIRE(out.size() == 6);

    CHECK(out[0].x == doctest::Approx(-60.0f));
    for (std::size_t i = 1; i < 6; ++i) {
        CHECK(out[i].x == doctest::Approx(60.0f));
    }

    // The 5 clustered ones have distinct y values.
    std::set<float> ys;
    for (std::size_t i = 1; i < 6; ++i) ys.insert(out[i].y);
    CHECK(ys.size() == 5);
}

TEST_CASE("timeline: a 5-node column is centered at y=0 with explicit spacing") {
    // Same setup but the cluster is at the start; verify the exact stack
    // ys against the column_spacing parameter so the algorithm's geometry
    // is pinned.
    const auto out = compute_timeline_positions(
        {100.0, 100.0, 100.0, 100.0, 100.0, 500.0},
        60.0f, 0.0f, /*column_spacing=*/4.0f);
    REQUIRE(out.size() == 6);

    // Indices 0..4 share t=100 -> stacked at x=-60.
    for (std::size_t i = 0; i < 5; ++i) CHECK(out[i].x == doctest::Approx(-60.0f));
    CHECK(out[5].x == doctest::Approx(60.0f));

    // Deterministic slot assignment by ascending index -> ys are
    // {-8, -4, 0, +4, +8} for spacing=4.
    CHECK(out[0].y == doctest::Approx(-8.0f));
    CHECK(out[1].y == doctest::Approx(-4.0f));
    CHECK(out[2].y == doctest::Approx( 0.0f));
    CHECK(out[3].y == doctest::Approx( 4.0f));
    CHECK(out[4].y == doctest::Approx( 8.0f));
    // Centroid of the stack is at y=0.
    const float mean = (out[0].y + out[1].y + out[2].y + out[3].y + out[4].y) / 5.0f;
    CHECK(mean == doctest::Approx(0.0f));
}

TEST_CASE("timeline: adjacent stacked nodes are at least column_spacing apart") {
    // 20 nodes all at t=100, plus one at t=500 to keep the all_equal branch
    // out of play. With column_spacing=3, neighbours in the stack should
    // be exactly 3 worldspace units apart.
    std::vector<double> ts(20, 100.0);
    ts.push_back(500.0);
    const auto out = compute_timeline_positions(ts, 60.0f, 0.0f, 3.0f);
    REQUIRE(out.size() == 21);

    std::vector<float> ys;
    for (std::size_t i = 0; i < 20; ++i) ys.push_back(out[i].y);
    std::sort(ys.begin(), ys.end());
    for (std::size_t i = 1; i < ys.size(); ++i) {
        const float gap = ys[i] - ys[i - 1];
        CHECK(gap >= 3.0f - 1e-3f);
    }
}

TEST_CASE("timeline: column stacking is deterministic across runs") {
    const std::vector<double> ts = {100.0, 100.0, 100.0, 100.0, 500.0};
    const auto a = compute_timeline_positions(ts, 60.0f, 10.0f, 3.0f);
    const auto b = compute_timeline_positions(ts, 60.0f, 10.0f, 3.0f);
    REQUIRE(a.size() == b.size());
    for (std::size_t i = 0; i < a.size(); ++i) {
        CHECK(a[i].x == doctest::Approx(b[i].x));
        CHECK(a[i].y == doctest::Approx(b[i].y));
    }
}

TEST_CASE("timeline: single-node-per-x columns still get the hash-based jitter") {
    // 5 distinct timestamps -> 5 separate single-node columns. None
    // stack, so each gets the legacy hash-based jitter in
    // [-y_jitter, +y_jitter] and not all of them collapse to 0.
    const auto out = compute_timeline_positions(
        {100.0, 200.0, 300.0, 400.0, 500.0}, 60.0f, 10.0f, 3.0f);
    REQUIRE(out.size() == 5);
    std::set<float> ys;
    for (const auto& p : out) {
        CHECK(p.y >= -10.0f);
        CHECK(p.y <=  10.0f);
        ys.insert(p.y);
    }
    // Hash jitter should produce more than one distinct y across 5 nodes.
    CHECK(ys.size() >= 2);
}

TEST_CASE("timeline: two separate clusters stack independently with no cross-talk") {
    // Cluster A at t=100 (3 nodes), gap, cluster B at t=500 (4 nodes).
    const auto out = compute_timeline_positions(
        {100.0, 100.0, 100.0, 500.0, 500.0, 500.0, 500.0},
        60.0f, 0.0f, /*column_spacing=*/3.0f);
    REQUIRE(out.size() == 7);

    // Cluster A: indices 0..2 at x=-60, ys = {-3, 0, +3}.
    for (std::size_t i = 0; i < 3; ++i) CHECK(out[i].x == doctest::Approx(-60.0f));
    CHECK(out[0].y == doctest::Approx(-3.0f));
    CHECK(out[1].y == doctest::Approx( 0.0f));
    CHECK(out[2].y == doctest::Approx( 3.0f));

    // Cluster B: indices 3..6 at x=+60, ys = {-4.5, -1.5, +1.5, +4.5}.
    for (std::size_t i = 3; i < 7; ++i) CHECK(out[i].x == doctest::Approx(60.0f));
    CHECK(out[3].y == doctest::Approx(-4.5f));
    CHECK(out[4].y == doctest::Approx(-1.5f));
    CHECK(out[5].y == doctest::Approx( 1.5f));
    CHECK(out[6].y == doctest::Approx( 4.5f));
}
