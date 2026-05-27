#include "physics/physics_thread.h"

#include <raylib.h>

#include <algorithm>
#include <chrono>
#include <cmath>

#include "physics/barnes_hut.h"
#include "physics/forces.h"
#include "telemetry/phantom.h"

namespace zg::physics {

PhysicsThread::PhysicsThread(std::vector<Vector3> initial_positions,
                             std::vector<graph::Edge> edges,
                             graph::GraphBuffer& buffer,
                             telemetry::PhantomBuffer* phantom_buffer,
                             SimParams params)
    : positions_(std::move(initial_positions)),
      velocities_(positions_.size(), Vector3{0.0f, 0.0f, 0.0f}),
      edges_(std::move(edges)),
      buffer_(buffer),
      phantom_buffer_(phantom_buffer),
      params_(params),
      use_barnes_hut_(params.use_barnes_hut) {
    buffer_.set_edges(edges_);
    buffer_.publish_positions(positions_);
}

PhysicsThread::~PhysicsThread() {
    stop();
}

void PhysicsThread::start() {
    if (running_.exchange(true)) return;
    worker_ = std::thread(&PhysicsThread::run, this);
}

void PhysicsThread::stop() {
    if (!running_.exchange(false)) return;
    if (worker_.joinable()) worker_.join();
}

void PhysicsThread::enqueue_node(Vector3 position) {
    std::lock_guard<std::mutex> lock(pending_mu_);
    pending_additions_.push_back(position);
}

void PhysicsThread::enqueue_edge(graph::Edge edge) {
    std::lock_guard<std::mutex> lock(pending_mu_);
    pending_edges_.push_back(edge);
}

void PhysicsThread::run() {
    using clock = std::chrono::steady_clock;
    const auto tick = std::chrono::microseconds(1'000'000 / std::max(1, params_.target_hz));

    while (running_.load(std::memory_order_relaxed)) {
        const auto t0 = clock::now();
        step();
        buffer_.publish_positions(positions_);
        // Republish edges each tick so the buffer stays consistent with the
        // physics thread's view after enqueue_edge drains. Cost is one vector
        // copy at the buffer-set mutex (microseconds at this scale).
        buffer_.set_edges(edges_);

        const auto elapsed = clock::now() - t0;
        if (elapsed < tick) std::this_thread::sleep_for(tick - elapsed);
    }
}

void integrate_step(std::vector<Vector3>& positions,
                    std::vector<Vector3>& velocities,
                    const std::vector<graph::Edge>& edges,
                    const SimParams& params,
                    const std::vector<Vector3>& phantom_positions) {
    const std::size_t n = positions.size();
    std::vector<Vector3> forces(n, Vector3{0.0f, 0.0f, 0.0f});

    if (params.use_barnes_hut) {
        // Tree-accelerated O(N log N) Coulomb pass.
        apply_barnes_hut_repulsion(positions, forces, params.repulsion_k, params.bh_theta);
    } else {
        // Naive O(N^2) pairwise — preserved for the off-toggle path and for
        // small-N tests where the constant factor of building a tree isn't
        // worth it.
        for (std::size_t i = 0; i < n; ++i) {
            for (std::size_t j = i + 1; j < n; ++j) {
                const Vector3 f = coulomb_force(positions[i], positions[j], 1.0f, 1.0f, params.repulsion_k);
                forces[i].x += f.x; forces[i].y += f.y; forces[i].z += f.z;
                forces[j].x -= f.x; forces[j].y -= f.y; forces[j].z -= f.z;
            }
        }
    }

    // Hooke attraction along edges.
    for (const graph::Edge& e : edges) {
        const Vector3 f = hooke_force(positions[e.source], positions[e.target],
                                       params.spring_rest, params.spring_k);
        forces[e.source].x += f.x; forces[e.source].y += f.y; forces[e.source].z += f.z;
        forces[e.target].x -= f.x; forces[e.target].y -= f.y; forces[e.target].z -= f.z;
    }

    // Centering pull toward origin. Edges only constrain the ~60 endpoints out
    // of 500, so without this, nodes with no springs drift outward indefinitely
    // under net Coulomb repulsion.
    for (std::size_t i = 0; i < n; ++i) {
        forces[i].x -= params.center_k * positions[i].x;
        forces[i].y -= params.center_k * positions[i].y;
        forces[i].z -= params.center_k * positions[i].z;
    }

    // One-way phantom repulsion: each phantom shoves nearby static nodes
    // with a much stronger Coulomb constant than static-vs-static. Phantoms
    // do NOT accumulate reaction force — they're anchored at telemetry
    // coordinates and never integrate.
    for (const Vector3& ph : phantom_positions) {
        for (std::size_t i = 0; i < n; ++i) {
            const Vector3 f = coulomb_force(positions[i], ph, 1.0f, 1.0f, params.phantom_repulsion_k);
            forces[i].x += f.x;
            forces[i].y += f.y;
            forces[i].z += f.z;
        }
    }

    // Symplectic Euler integration with velocity damping and a hard speed cap.
    for (std::size_t i = 0; i < n; ++i) {
        velocities[i].x = (velocities[i].x + forces[i].x * params.dt) * params.damping;
        velocities[i].y = (velocities[i].y + forces[i].y * params.dt) * params.damping;
        velocities[i].z = (velocities[i].z + forces[i].z * params.dt) * params.damping;

        const float speed2 = velocities[i].x * velocities[i].x
                           + velocities[i].y * velocities[i].y
                           + velocities[i].z * velocities[i].z;
        if (speed2 > params.max_speed * params.max_speed) {
            const float scale = params.max_speed / std::sqrt(speed2);
            velocities[i].x *= scale;
            velocities[i].y *= scale;
            velocities[i].z *= scale;
        }

        positions[i].x += velocities[i].x * params.dt;
        positions[i].y += velocities[i].y * params.dt;
        positions[i].z += velocities[i].z * params.dt;
    }
}

void PhysicsThread::step() {
    // Drain queued node and edge additions before integrating. Each new node
    // enters at zero velocity; the integrator picks both up on the same tick.
    {
        std::lock_guard<std::mutex> lock(pending_mu_);
        for (const Vector3& pos : pending_additions_) {
            positions_.push_back(pos);
            velocities_.push_back({0.0f, 0.0f, 0.0f});
        }
        pending_additions_.clear();

        for (const graph::Edge& e : pending_edges_) {
            edges_.push_back(e);
        }
        pending_edges_.clear();
    }

    std::vector<Vector3> phantom_positions;
    if (phantom_buffer_) {
        std::vector<telemetry::Phantom> phantoms;
        phantom_buffer_->snapshot_and_expire(phantoms, params_.phantom_ttl, GetTime());
        phantom_positions.reserve(phantoms.size());
        for (const auto& p : phantoms) phantom_positions.push_back(p.position);
    }

    // Pull the runtime toggle through to SimParams so the integrator picks
    // up the latest setting without a restart.
    SimParams effective    = params_;
    effective.use_barnes_hut = use_barnes_hut_.load();
    integrate_step(positions_, velocities_, edges_, effective, phantom_positions);
}

}  // namespace zg::physics
