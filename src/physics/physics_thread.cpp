#include "physics/physics_thread.h"

#include <algorithm>
#include <chrono>
#include <cmath>

#include "physics/forces.h"

namespace zg::physics {

PhysicsThread::PhysicsThread(std::vector<Vector3> initial_positions,
                             std::vector<graph::Edge> edges,
                             graph::GraphBuffer& buffer,
                             SimParams params)
    : positions_(std::move(initial_positions)),
      velocities_(positions_.size(), Vector3{0.0f, 0.0f, 0.0f}),
      edges_(std::move(edges)),
      buffer_(buffer),
      params_(params) {
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

void PhysicsThread::run() {
    using clock = std::chrono::steady_clock;
    const auto tick = std::chrono::microseconds(1'000'000 / std::max(1, params_.target_hz));

    while (running_.load(std::memory_order_relaxed)) {
        const auto t0 = clock::now();
        step();
        buffer_.publish_positions(positions_);

        const auto elapsed = clock::now() - t0;
        if (elapsed < tick) std::this_thread::sleep_for(tick - elapsed);
    }
}

void PhysicsThread::step() {
    const std::size_t n = positions_.size();
    std::vector<Vector3> forces(n, Vector3{0.0f, 0.0f, 0.0f});

    // Pairwise Coulomb repulsion (O(N^2) — naive; Barnes-Hut comes later).
    for (std::size_t i = 0; i < n; ++i) {
        for (std::size_t j = i + 1; j < n; ++j) {
            const Vector3 f = coulomb_force(positions_[i], positions_[j], 1.0f, 1.0f, params_.repulsion_k);
            forces[i].x += f.x; forces[i].y += f.y; forces[i].z += f.z;
            forces[j].x -= f.x; forces[j].y -= f.y; forces[j].z -= f.z;
        }
    }

    // Hooke attraction along edges.
    for (const graph::Edge& e : edges_) {
        const Vector3 f = hooke_force(positions_[e.source], positions_[e.target],
                                       params_.spring_rest, params_.spring_k);
        forces[e.source].x += f.x; forces[e.source].y += f.y; forces[e.source].z += f.z;
        forces[e.target].x -= f.x; forces[e.target].y -= f.y; forces[e.target].z -= f.z;
    }

    // Centering pull toward origin. Edges only constrain the ~60 endpoints out
    // of 500, so without this, nodes with no springs drift outward indefinitely
    // under net Coulomb repulsion.
    for (std::size_t i = 0; i < n; ++i) {
        forces[i].x -= params_.center_k * positions_[i].x;
        forces[i].y -= params_.center_k * positions_[i].y;
        forces[i].z -= params_.center_k * positions_[i].z;
    }

    // Symplectic Euler integration with velocity damping and a hard speed cap.
    for (std::size_t i = 0; i < n; ++i) {
        velocities_[i].x = (velocities_[i].x + forces[i].x * params_.dt) * params_.damping;
        velocities_[i].y = (velocities_[i].y + forces[i].y * params_.dt) * params_.damping;
        velocities_[i].z = (velocities_[i].z + forces[i].z * params_.dt) * params_.damping;

        const float speed2 = velocities_[i].x * velocities_[i].x
                           + velocities_[i].y * velocities_[i].y
                           + velocities_[i].z * velocities_[i].z;
        if (speed2 > params_.max_speed * params_.max_speed) {
            const float scale = params_.max_speed / std::sqrt(speed2);
            velocities_[i].x *= scale;
            velocities_[i].y *= scale;
            velocities_[i].z *= scale;
        }

        positions_[i].x += velocities_[i].x * params_.dt;
        positions_[i].y += velocities_[i].y * params_.dt;
        positions_[i].z += velocities_[i].z * params_.dt;
    }
}

}  // namespace zg::physics
