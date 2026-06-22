#pragma once

#include <raylib.h>

#include <atomic>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
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
    // Freeze-on-convergence: once the graph's RMS node speed stays below
    // rest_speed_eps for freeze_after_ticks consecutive ticks, the layout has
    // essentially stopped rearranging and the thread pauses the (expensive)
    // integrate step until something perturbs it — a new node/edge, a pin
    // change, or a live phantom. This is what keeps a big graph from burning
    // multi-second Barnes-Hut ticks forever (and a reloaded, already-settled
    // graph starts at ~0 velocity, so it freezes almost at once). The
    // threshold is whole-graph RMS, not a max, so a few slow stragglers don't
    // pin the sim awake. Set freeze_after_ticks <= 0 to disable.
    float rest_speed_eps      = 1.0f;    // RMS units/sec — bulk layout done, only slow drift left
    int   freeze_after_ticks  = 60;      // ~0.5 s at 120 Hz
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
//
// `disabled` (empty == none) masks out tombstoned nodes: a disabled node exerts
// no repulsion, isn't pulled by edges/centering/phantoms, and is frozen in
// place with zero velocity — so a soft-deleted node stops dragging the layout.
void integrate_step(std::vector<Vector3>& positions,
                    std::vector<Vector3>& velocities,
                    const std::vector<graph::Edge>& edges,
                    const SimParams& params,
                    const std::vector<Vector3>& phantom_positions = {},
                    const std::vector<char>& disabled = {});

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

    // Marks a node as tombstoned: from the next tick it exerts and receives no
    // force and freezes in place (see integrate_step's disabled mask). Called
    // from the render thread when a node is soft-deleted. Thread-safe; one-way
    // (soft-delete is permanent — there's no re-enable).
    void set_node_disabled(std::size_t idx);

    // Toggle Barnes-Hut at runtime. Cheap, lock-free, takes effect on the
    // next tick. False switches back to the naive O(N^2) Coulomb loop.
    void set_use_barnes_hut(bool use) { use_barnes_hut_.store(use); }
    bool use_barnes_hut() const       { return use_barnes_hut_.load(); }

private:
    void run();

    // One simulation tick. Always does the cheap work (drain pending node/edge
    // additions, snapshot phantoms, re-apply pins); runs the expensive
    // integrate step only when awake — i.e. not `frozen`, or perturbed this
    // tick by a live phantom or a freshly-drained addition.
    struct StepResult {
        bool integrated = false;  // did the expensive integrate step run this tick?
        bool active     = false;  // a phantom or new addition perturbed the field
    };
    StepResult step(bool frozen);

    std::vector<Vector3>          positions_;
    std::vector<Vector3>          velocities_;
    std::vector<graph::Edge>      edges_;
    graph::GraphBuffer&           buffer_;
    telemetry::PhantomBuffer*     phantom_buffer_;
    SimParams                     params_;
    // Set by step() when an enqueue_edge drain mutated edges_; run()
    // republishes to the buffer and clears it. Physics-thread-only state,
    // no synchronization needed.
    bool                          edges_dirty_ = false;

    std::atomic<bool>          running_{false};
    std::atomic<bool>          use_barnes_hut_;
    // Set by enqueue_node/edge + set_pin/clear_pin to wake a frozen sim on the
    // next tick. The worker clears it; perturbations from any thread are safe.
    std::atomic<bool>          dirty_{false};
    std::thread                worker_;
    std::mutex                 pending_mu_;
    std::vector<Vector3>       pending_additions_;
    std::vector<graph::Edge>   pending_edges_;

    // Pinned nodes — index -> anchor position. Re-applied after every
    // integrate_step under pins_mu_ separately so a pin update isn't
    // serialized behind the pending-additions drain.
    mutable std::mutex                                  pins_mu_;
    std::unordered_map<std::size_t, Vector3>            pins_;

    // Tombstoned (soft-deleted) node indices, set by set_node_disabled from the
    // render thread. step() builds a per-tick mask from this so deleted nodes
    // drop out of the force calculation. Usually tiny (a few tombstones).
    mutable std::mutex                                  disabled_mu_;
    std::unordered_set<std::size_t>                     disabled_;
};

}  // namespace zg::physics
