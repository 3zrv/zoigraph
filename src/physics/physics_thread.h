#pragma once

#include <raylib.h>

#include <atomic>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include "graph/graph_buffer.h"
#include "graph/types.h"
#include "telemetry/phantom_buffer.h"

namespace zg::physics {

struct SimParams {
    float repulsion_k         = 80.0f;   // Coulomb constant for static-vs-static
    float phantom_repulsion_k = 800.0f;  // 10x heavier — phantoms violently shove static nodes (§5.B)
    float phantom_ttl         = 60.0f;   // seconds (§5.B); physics thread uses this to expire on snapshot
    float spring_rest         = 4.0f;
    float spring_k            = 0.05f;
    float center_k            = 0.5f;    // linear pull toward origin, keeps cluster bounded
    float damping             = 0.85f;
    float dt                  = 0.05f;
    float max_speed           = 20.0f;   // velocity clamp to prevent blow-up
    int   target_hz           = 120;     // physics tick rate
    bool  use_barnes_hut      = true;    // O(N log N) octree approximation for the Coulomb pass
    float bh_theta            = 0.7f;    // Barnes-Hut opening angle
};

// One step of the force-directed integration: sum pairwise Coulomb + per-edge
// Hooke + centering + one-way phantom-repulsion, integrate with damping +
// speed cap. Pure (apart from mutating the passed-in vectors), so it's
// directly unit-testable without a thread or a render loop.
//
// `phantom_positions` defaults to empty for tests / callers that don't care
// about telemetry; when populated, each phantom applies Coulomb repulsion to
// every static node but does not itself accumulate reaction force (phantoms
// stay anchored at telemetry coordinates).
void integrate_step(std::vector<Vector3>& positions,
                    std::vector<Vector3>& velocities,
                    const std::vector<graph::Edge>& edges,
                    const SimParams& params,
                    const std::vector<Vector3>& phantom_positions = {});

// Owns the simulation state and a background std::thread. Publishes positions
// to a GraphBuffer that the render thread reads each frame.
class PhysicsThread {
public:
    PhysicsThread(std::vector<Vector3> initial_positions,
                  std::vector<graph::Edge> edges,
                  graph::GraphBuffer& buffer,
                  telemetry::PhantomBuffer* phantom_buffer = nullptr,
                  SimParams params = {});
    ~PhysicsThread();

    PhysicsThread(const PhysicsThread&) = delete;
    PhysicsThread& operator=(const PhysicsThread&) = delete;

    void start();
    void stop();

    // Queues a new static node to be added to the simulation at the start of
    // the next tick. Thread-safe — caller need not coordinate with the
    // running physics loop. The node starts at `position` with zero velocity.
    void enqueue_node(Vector3 position);

    // Queues a new edge to be added to the simulation at the start of the
    // next tick. Used when a pinned phantom carries `connections` that should
    // become permanent springs in the graph. Thread-safe.
    void enqueue_edge(graph::Edge edge);

    // Pins a node at `anchor`: after each integration step the node's
    // position is restored to the anchor and its velocity zeroed.  Used
    // for the `self` node ("the operator") so the rest of the graph
    // arranges itself relative to a fixed center. Thread-safe.
    void set_pin(std::size_t idx, Vector3 anchor);

    // Removes the pin on a node. Subsequent ticks let physics move it
    // again. No-op if the node wasn't pinned. Thread-safe.
    void clear_pin(std::size_t idx);

    // Toggle Barnes-Hut at runtime. Cheap, lock-free, takes effect on the
    // next tick. False switches back to the naive O(N^2) Coulomb loop.
    void set_use_barnes_hut(bool use) { use_barnes_hut_.store(use); }
    bool use_barnes_hut() const       { return use_barnes_hut_.load(); }

private:
    void run();
    void step();

    std::vector<Vector3>          positions_;
    std::vector<Vector3>          velocities_;
    std::vector<graph::Edge>      edges_;
    graph::GraphBuffer&           buffer_;
    telemetry::PhantomBuffer*     phantom_buffer_;
    SimParams                     params_;

    std::atomic<bool>          running_{false};
    std::atomic<bool>          use_barnes_hut_;
    std::thread                worker_;
    std::mutex                 pending_mu_;
    std::vector<Vector3>       pending_additions_;
    std::vector<graph::Edge>   pending_edges_;

    // Pinned nodes — index -> anchor position. Re-applied after every
    // integrate_step under pins_mu_ separately so a pin update isn't
    // serialized behind the pending-additions drain.
    mutable std::mutex                                  pins_mu_;
    std::unordered_map<std::size_t, Vector3>            pins_;
};

}  // namespace zg::physics
