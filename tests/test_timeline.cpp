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

TEST_CASE("timeline: jitter on y AND z stays within the configured range") {
    const auto out = compute_timeline_positions(
        {100.0, 200.0, 300.0, 400.0, 500.0}, 60.0f, /*y_jitter=*/10.0f);
    REQUIRE(out.size() == 5);
    for (const auto& p : out) {
        CHECK(p.y >= -10.0f); CHECK(p.y <= 10.0f);
        CHECK(p.z >= -10.0f); CHECK(p.z <= 10.0f);
    }
}

TEST_CASE("timeline: single-node columns get z jitter too, so layout isn't flat") {
    // Distinct timestamps so every node is its own single-node column.
    // With y_jitter > 0, at least some nodes must have z != 0 -- otherwise
    // the timeline view becomes a flat ribbon in the x-y plane.
    const auto out = compute_timeline_positions(
        {100.0, 200.0, 300.0, 400.0, 500.0}, 60.0f, /*y_jitter=*/10.0f);
    bool any_z_nonzero = false;
    for (const auto& p : out) {
        if (std::fabs(p.z) > 1e-3f) { any_z_nonzero = true; break; }
    }
    CHECK(any_z_nonzero);
}

TEST_CASE("timeline: y and z jitter are independent (not the same value)") {
    const auto out = compute_timeline_positions(
        {100.0, 200.0, 300.0, 400.0, 500.0}, 60.0f, 10.0f);
    int distinct_axes = 0;
    for (const auto& p : out) {
        if (std::fabs(p.y - p.z) > 1e-3f) ++distinct_axes;
    }
    // Most nodes should have y != z; if they were the same hash, they'd
    // all line up on a y=z diagonal which is no better than a 1D line.
    CHECK(distinct_axes >= 4);
}

TEST_CASE("timeline: jitter is deterministic per index in both y and z") {
    const auto a = compute_timeline_positions({100.0, 200.0, 300.0}, 60.0f, 10.0f);
    const auto b = compute_timeline_positions({100.0, 200.0, 300.0}, 60.0f, 10.0f);
    REQUIRE(a.size() == b.size());
    for (std::size_t i = 0; i < a.size(); ++i) {
        CHECK(a[i].y == doctest::Approx(b[i].y));
        CHECK(a[i].z == doctest::Approx(b[i].z));
    }
}

TEST_CASE("timeline: y_jitter=0 zeroes both y and z (clean axis layout when requested)") {
    const auto out = compute_timeline_positions(
        {100.0, 200.0, 300.0}, 60.0f, /*y_jitter=*/0.0f);
    for (const auto& p : out) {
        CHECK(p.y == doctest::Approx(0.0f));
        CHECK(p.z == doctest::Approx(0.0f));
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

// ---- Column packing in the y-z plane when nodes cluster at the same x ----

TEST_CASE("timeline: 5 nodes at the same late timestamp share x but spread in y-z") {
    // 1 node at t=100, 5 nodes at t=500. The 5 clustered at the latest
    // end should share the same x (rightmost) but each get a distinct
    // (y,z) so they don't pile on top of each other in any 2D projection.
    const auto out = compute_timeline_positions(
        {100.0, 500.0, 500.0, 500.0, 500.0, 500.0},
        /*x_span=*/60.0f, /*y_jitter=*/10.0f, /*column_spacing=*/3.0f);
    REQUIRE(out.size() == 6);

    CHECK(out[0].x == doctest::Approx(-60.0f));
    for (std::size_t i = 1; i < 6; ++i) {
        CHECK(out[i].x == doctest::Approx(60.0f));
    }

    // All 5 clustered slots are at distinct (y, z) positions.
    std::set<std::pair<int, int>> seen;
    for (std::size_t i = 1; i < 6; ++i) {
        // Quantise to avoid float-equality fuzz when inserting.
        seen.insert({static_cast<int>(out[i].y * 1000.0f),
                     static_cast<int>(out[i].z * 1000.0f)});
    }
    CHECK(seen.size() == 5);
}

TEST_CASE("timeline: slot 0 of a cluster lands at the cluster centre (y=z=0)") {
    const auto out = compute_timeline_positions(
        {100.0, 100.0, 100.0, 100.0, 100.0, 500.0},
        60.0f, 0.0f, /*column_spacing=*/4.0f);
    REQUIRE(out.size() == 6);

    // First slot at the cluster's centre.
    CHECK(out[0].x == doctest::Approx(-60.0f));
    CHECK(out[0].y == doctest::Approx(0.0f));
    CHECK(out[0].z == doctest::Approx(0.0f));

    // Slots 1..4 are out on the spiral -- distance from origin grows as
    // sqrt(slot) * column_spacing. For column_spacing=4 the predicted
    // radii are {4, 4*sqrt(2), 4*sqrt(3), 8}.
    for (std::size_t slot = 1; slot < 5; ++slot) {
        const float r = std::sqrt(out[slot].y * out[slot].y
                                  + out[slot].z * out[slot].z);
        const float expected =
            4.0f * std::sqrt(static_cast<float>(slot));
        CHECK(r == doctest::Approx(expected).epsilon(0.001));
    }
}

TEST_CASE("timeline: 20 same-timestamp nodes pack into a disc, not a line") {
    // The key visual property: a many-node cluster should NOT collapse
    // onto a single straight line in any axis. Verify by checking that
    // the cluster has meaningful extent in BOTH y and z.
    std::vector<double> ts(20, 100.0);
    ts.push_back(500.0);
    const auto out = compute_timeline_positions(ts, 60.0f, 0.0f, 3.0f);
    REQUIRE(out.size() == 21);

    float min_y = 1e9f, max_y = -1e9f;
    float min_z = 1e9f, max_z = -1e9f;
    for (std::size_t i = 0; i < 20; ++i) {
        min_y = std::min(min_y, out[i].y);
        max_y = std::max(max_y, out[i].y);
        min_z = std::min(min_z, out[i].z);
        max_z = std::max(max_z, out[i].z);
    }
    const float y_extent = max_y - min_y;
    const float z_extent = max_z - min_z;
    CHECK(y_extent > 5.0f);   // not a degenerate vertical line
    CHECK(z_extent > 5.0f);   // not a degenerate horizontal line
}

TEST_CASE("timeline: all positions in a cluster are pairwise distinct") {
    std::vector<double> ts(20, 100.0);
    ts.push_back(500.0);
    const auto out = compute_timeline_positions(ts, 60.0f, 0.0f, 3.0f);
    std::set<std::pair<int, int>> positions;
    for (std::size_t i = 0; i < 20; ++i) {
        positions.insert({static_cast<int>(out[i].y * 1000.0f),
                          static_cast<int>(out[i].z * 1000.0f)});
    }
    CHECK(positions.size() == 20);
}

TEST_CASE("timeline: column packing is deterministic across runs") {
    const std::vector<double> ts = {100.0, 100.0, 100.0, 100.0, 500.0};
    const auto a = compute_timeline_positions(ts, 60.0f, 10.0f, 3.0f);
    const auto b = compute_timeline_positions(ts, 60.0f, 10.0f, 3.0f);
    REQUIRE(a.size() == b.size());
    for (std::size_t i = 0; i < a.size(); ++i) {
        CHECK(a[i].x == doctest::Approx(b[i].x));
        CHECK(a[i].y == doctest::Approx(b[i].y));
        CHECK(a[i].z == doctest::Approx(b[i].z));
    }
}

TEST_CASE("timeline: single-node-per-x columns still get the hash-based jitter") {
    // 5 distinct timestamps -> 5 separate single-node columns. None
    // cluster, so each gets the legacy y jitter in [-y_jitter, +y_jitter]
    // with z=0.
    const auto out = compute_timeline_positions(
        {100.0, 200.0, 300.0, 400.0, 500.0}, 60.0f, 10.0f, 3.0f);
    REQUIRE(out.size() == 5);
    std::set<float> ys;
    for (const auto& p : out) {
        CHECK(p.y >= -10.0f);
        CHECK(p.y <=  10.0f);
        CHECK(p.z >= -10.0f);
        CHECK(p.z <=  10.0f);
        ys.insert(p.y);
    }
    CHECK(ys.size() >= 2);
}

TEST_CASE("timeline: two separate clusters pack independently with no cross-talk") {
    // Cluster A at t=100 (3 nodes), cluster B at t=500 (4 nodes). Each
    // should have its slot-0 at its own centre (0,0) on its own x axis.
    const auto out = compute_timeline_positions(
        {100.0, 100.0, 100.0, 500.0, 500.0, 500.0, 500.0},
        60.0f, 0.0f, /*column_spacing=*/3.0f);
    REQUIRE(out.size() == 7);

    for (std::size_t i = 0; i < 3; ++i) CHECK(out[i].x == doctest::Approx(-60.0f));
    for (std::size_t i = 3; i < 7; ++i) CHECK(out[i].x == doctest::Approx(60.0f));

    // Each cluster's slot-0 is at (y=0, z=0).
    CHECK(out[0].y == doctest::Approx(0.0f));
    CHECK(out[0].z == doctest::Approx(0.0f));
    CHECK(out[3].y == doctest::Approx(0.0f));
    CHECK(out[3].z == doctest::Approx(0.0f));

    // Cluster B's last slot (slot 3 within its column) sits at radius
    // = column_spacing * sqrt(3) ≈ 5.196 from its centre.
    const float r_last = std::sqrt(out[6].y * out[6].y + out[6].z * out[6].z);
    CHECK(r_last == doctest::Approx(3.0f * std::sqrt(3.0f)).epsilon(0.001));
}
