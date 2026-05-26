#pragma once

#include <raylib.h>

#include <atomic>
#include <thread>
#include <vector>

#include "graph/graph_buffer.h"
#include "graph/types.h"

namespace zg::physics {

struct SimParams {
    float repulsion_k   = 80.0f;  // Coulomb constant
    float spring_rest   = 4.0f;
    float spring_k      = 0.05f;
    float center_k      = 0.5f;   // linear pull toward origin, keeps cluster bounded
    float damping       = 0.85f;
    float dt            = 0.05f;
    float max_speed     = 20.0f;  // velocity clamp to prevent blow-up
    int   target_hz     = 120;    // physics tick rate
};

// Owns the simulation state and a background std::thread. Publishes positions
// to a GraphBuffer that the render thread reads each frame.
class PhysicsThread {
public:
    PhysicsThread(std::vector<Vector3> initial_positions,
                  std::vector<graph::Edge> edges,
                  graph::GraphBuffer& buffer,
                  SimParams params = {});
    ~PhysicsThread();

    PhysicsThread(const PhysicsThread&) = delete;
    PhysicsThread& operator=(const PhysicsThread&) = delete;

    void start();
    void stop();

private:
    void run();
    void step();

    std::vector<Vector3>      positions_;
    std::vector<Vector3>      velocities_;
    std::vector<graph::Edge>  edges_;
    graph::GraphBuffer&       buffer_;
    SimParams                 params_;

    std::atomic<bool> running_{false};
    std::thread       worker_;
};

}  // namespace zg::physics
