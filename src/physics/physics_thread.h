#pragma once

#include <raylib.h>

#include <atomic>
#include <mutex>
#include <thread>
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

private:
    void run();
    void step();

    std::vector<Vector3>          positions_;
    std::vector<Vector3>          velocities_;
    std::vector<graph::Edge>      edges_;
    graph::GraphBuffer&           buffer_;
    telemetry::PhantomBuffer*     phantom_buffer_;
    SimParams                     params_;

    std::atomic<bool>      running_{false};
    std::thread            worker_;
    std::mutex             pending_mu_;
    std::vector<Vector3>   pending_additions_;
};

}  // namespace zg::physics
