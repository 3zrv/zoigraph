#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "graph/cluster.h"
#include "graph/types.h"

#include <set>

using zg::graph::Edge;
using zg::graph::label_propagation;

TEST_CASE("label_propagation: empty graph returns empty") {
    CHECK(label_propagation(0, {}).empty());
}

TEST_CASE("label_propagation: single node keeps its initial label") {
    const auto labels = label_propagation(1, {});
    REQUIRE(labels.size() == 1);
    CHECK(labels[0] == 0);
}

TEST_CASE("label_propagation: isolated nodes keep their own labels") {
    // No edges between 5 nodes — every node is its own cluster.
    const auto labels = label_propagation(5, {});
    REQUIRE(labels.size() == 5);
    std::set<std::size_t> unique(labels.begin(), labels.end());
    CHECK(unique.size() == 5);
}

TEST_CASE("label_propagation: two nodes joined by one edge share a label") {
    const auto labels = label_propagation(2, {{0, 1}});
    REQUIRE(labels.size() == 2);
    CHECK(labels[0] == labels[1]);
}

TEST_CASE("label_propagation: K3 fully connected -> all 3 share one cluster") {
    const auto labels = label_propagation(3, {{0, 1}, {1, 2}, {0, 2}});
    REQUIRE(labels.size() == 3);
    CHECK(labels[0] == labels[1]);
    CHECK(labels[1] == labels[2]);
}

TEST_CASE("label_propagation: two disconnected components stay separate") {
    // Cluster A: 0-1-2 chain.  Cluster B: 3-4 edge.  No edge between them.
    const auto labels = label_propagation(5, {{0, 1}, {1, 2}, {3, 4}});
    REQUIRE(labels.size() == 5);
    CHECK(labels[0] == labels[1]);
    CHECK(labels[1] == labels[2]);
    CHECK(labels[3] == labels[4]);
    CHECK(labels[0] != labels[3]);
}

TEST_CASE("label_propagation: self-loops are ignored") {
    const auto labels = label_propagation(3, {{0, 0}, {1, 1}, {2, 2}});
    REQUIRE(labels.size() == 3);
    // With only self-loops, every node is effectively isolated -> 3 clusters.
    std::set<std::size_t> unique(labels.begin(), labels.end());
    CHECK(unique.size() == 3);
}

TEST_CASE("label_propagation: out-of-bounds edges are silently dropped") {
    const auto labels = label_propagation(3, {{0, 99}, {1, 2}, {500, 600}});
    REQUIRE(labels.size() == 3);
    // The 1-2 edge takes effect; 0 stays alone.
    CHECK(labels[1] == labels[2]);
    CHECK(labels[0] != labels[1]);
}

TEST_CASE("label_propagation: same input -> same output (deterministic)") {
    std::vector<Edge> edges = {{0, 1}, {1, 2}, {3, 4}, {4, 5}};
    const auto a = label_propagation(6, edges);
    const auto b = label_propagation(6, edges);
    REQUIRE(a.size() == b.size());
    for (std::size_t i = 0; i < a.size(); ++i) CHECK(a[i] == b[i]);
}

TEST_CASE("label_propagation: clusters of a barbell graph form correctly") {
    // K3 on {0,1,2}, K3 on {3,4,5}, single bridge edge 2-3. Expected:
    // either one giant cluster or two clusters split at the bridge,
    // depending on tie-breaking. With smallest-label tie-break this
    // converges to one cluster (the bridge propagates the smaller label).
    const auto labels = label_propagation(6, {
        {0, 1}, {0, 2}, {1, 2},
        {3, 4}, {3, 5}, {4, 5},
        {2, 3},
    });
    REQUIRE(labels.size() == 6);
    // Each triangle should at minimum be internally consistent.
    CHECK(labels[0] == labels[1]);
    CHECK(labels[1] == labels[2]);
    CHECK(labels[3] == labels[4]);
    CHECK(labels[4] == labels[5]);
}
