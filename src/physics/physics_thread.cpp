#include "physics/physics_thread.h"

#include <raylib.h>

#include <algorithm>
#include <chrono>
#include <cmath>

#include "app/clock.h"
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

// Every mutator sets dirty_ AFTER its mutation lands, so when the worker sees
// the flag the change is already visible to it (the pending-queue drain and
// pin re-apply both re-lock the same mutex). dirty_ wakes a frozen sim.
void PhysicsThread::enqueue_node(Vector3 position) {
    {
        std::lock_guard<std::mutex> lock(pending_mu_);
        pending_additions_.push_back(position);
    }
    dirty_.store(true, std::memory_order_release);
}

void PhysicsThread::enqueue_edge(graph::Edge edge) {
    {
        std::lock_guard<std::mutex> lock(pending_mu_);
        pending_edges_.push_back(edge);
    }
    dirty_.store(true, std::memory_order_release);
}

void PhysicsThread::set_pin(std::size_t idx, Vector3 anchor) {
    {
        std::lock_guard<std::mutex> lock(pins_mu_);
        pins_[idx] = anchor;
    }
    dirty_.store(true, std::memory_order_release);
}

void PhysicsThread::clear_pin(std::size_t idx) {
    {
        std::lock_guard<std::mutex> lock(pins_mu_);
        pins_.erase(idx);
    }
    dirty_.store(true, std::memory_order_release);
}

void PhysicsThread::set_node_disabled(std::size_t idx) {
    {
        std::lock_guard<std::mutex> lock(disabled_mu_);
        disabled_.insert(idx);
    }
    // Removing a node's repulsion/springs shifts the equilibrium — wake a
    // frozen sim so the rest of the graph re-settles without it.
    dirty_.store(true, std::memory_order_release);
}

void PhysicsThread::run() {
    using clock = std::chrono::steady_clock;
    const auto tick = std::chrono::microseconds(1'000'000 / std::max(1, params_.target_hz));

    const float eps2 = params_.rest_speed_eps * params_.rest_speed_eps;
    int  rest_ticks  = 0;       // consecutive at-rest ticks
    bool frozen      = false;   // when true, the expensive integrate is paused

    while (running_.load(std::memory_order_relaxed)) {
        const auto t0 = clock::now();

        // A perturbation since last tick (new node/edge, pin change) resumes
        // the sim regardless of how settled it looked.
        if (dirty_.exchange(false, std::memory_order_acquire)) {
            frozen     = false;
            rest_ticks = 0;
        }

        // step() always does the cheap work and snapshots phantoms; it only
        // runs the expensive integrate when awake. A frozen, unperturbed tick
        // therefore costs ~nothing (no integrate, no publish).
        const StepResult r = step(frozen);
        if (r.integrated) {
            buffer_.publish_positions(positions_);
            // Edges republish ONLY when an enqueue_edge drain changed them
            // (operator actions: pin, wikilink, journal -- rare). The buffer's
            // edge snapshot stays correct for anyone who does read it.
            if (edges_dirty_) {
                buffer_.set_edges(edges_);
                edges_dirty_ = false;
            }

            // Convergence accounting. A phantom or fresh addition keeps the
            // sim awake and re-settling; otherwise count at-rest ticks toward
            // a freeze. freeze_after_ticks <= 0 disables freezing entirely.
            if (r.active) {
                frozen     = false;
                rest_ticks = 0;
            } else if (params_.freeze_after_ticks > 0
                       && mean_speed_squared(velocities_) < eps2) {
                if (++rest_ticks >= params_.freeze_after_ticks) frozen = true;
            } else {
                rest_ticks = 0;
            }
        }

        const auto elapsed = clock::now() - t0;
        if (elapsed < tick) std::this_thread::sleep_for(tick - elapsed);
    }
}

void integrate_step(std::vector<Vector3>& positions,
                    std::vector<Vector3>& velocities,
                    const std::vector<graph::Edge>& edges,
                    const SimParams& params,
                    const std::vector<Vector3>& phantom_positions,
                    const std::vector<char>& disabled) {
    const std::size_t n = positions.size();
    std::vector<Vector3> forces(n, Vector3{0.0f, 0.0f, 0.0f});

    // Tombstoned nodes (empty mask == none) exert and receive no force and
    // never integrate — a soft-deleted node must stop dragging the layout.
    const auto off = [&disabled](std::size_t i) {
        return i < disabled.size() && disabled[i];
    };

    if (params.use_barnes_hut) {
        // Tree-accelerated O(N log N) Coulomb pass.
        apply_barnes_hut_repulsion(positions, forces, params.repulsion_k,
                                   params.bh_theta, disabled);
    } else {
        // Naive O(N^2) pairwise — preserved for the off-toggle path and for
        // small-N tests where the constant factor of building a tree isn't
        // worth it.
        for (std::size_t i = 0; i < n; ++i) {
            if (off(i)) continue;
            for (std::size_t j = i + 1; j < n; ++j) {
                if (off(j)) continue;
                const Vector3 f = coulomb_force(positions[i], positions[j], 1.0f, 1.0f, params.repulsion_k);
                forces[i].x += f.x; forces[i].y += f.y; forces[i].z += f.z;
                forces[j].x -= f.x; forces[j].y -= f.y; forces[j].z -= f.z;
            }
        }
    }

    // Hooke attraction along edges. Out-of-bounds edges (e.g. orphans loaded
    // from a DB that drifted out of sync with nodes) are silently skipped
    // rather than read past the end of positions/forces. Edges touching a
    // disabled endpoint are skipped too — a tombstone's springs don't pull.
    for (const graph::Edge& e : edges) {
        if (e.source >= n || e.target >= n) continue;
        if (off(e.source) || off(e.target)) continue;
        const Vector3 f = hooke_force(positions[e.source], positions[e.target],
                                       params.spring_rest, params.spring_k);
        forces[e.source].x += f.x; forces[e.source].y += f.y; forces[e.source].z += f.z;
        forces[e.target].x -= f.x; forces[e.target].y -= f.y; forces[e.target].z -= f.z;
    }

    // Centering pull toward origin. Edges only constrain the ~60 endpoints out
    // of 500, so without this, nodes with no springs drift outward indefinitely
    // under net Coulomb repulsion.
    for (std::size_t i = 0; i < n; ++i) {
        if (off(i)) continue;
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
            if (off(i)) continue;
            const Vector3 f = coulomb_force(positions[i], ph, 1.0f, 1.0f, params.phantom_repulsion_k);
            forces[i].x += f.x;
            forces[i].y += f.y;
            forces[i].z += f.z;
        }
    }

    // Symplectic Euler integration with velocity damping and a hard speed cap.
    for (std::size_t i = 0; i < n; ++i) {
        if (off(i)) {  // frozen in place, zero velocity
            velocities[i] = {0.0f, 0.0f, 0.0f};
            continue;
        }
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

PhysicsThread::StepResult PhysicsThread::step(bool frozen) {
    // --- cheap work, every tick (so a frozen sim still notices changes) ---

    // Drain queued node and edge additions. Each new node enters at zero
    // velocity; the integrator picks both up on the same tick.
    bool added = false;
    {
        std::lock_guard<std::mutex> lock(pending_mu_);
        for (const Vector3& pos : pending_additions_) {
            positions_.push_back(pos);
            velocities_.push_back({0.0f, 0.0f, 0.0f});
            added = true;
        }
        pending_additions_.clear();

        if (!pending_edges_.empty()) { edges_dirty_ = true; added = true; }
        for (const graph::Edge& e : pending_edges_) {
            edges_.push_back(e);
        }
        pending_edges_.clear();
    }

    // Snapshot phantoms even when frozen: a phantom that appears must wake the
    // sim so its repulsion is actually applied. Cheap — phantoms are few.
    std::vector<Vector3> phantom_positions;
    if (phantom_buffer_) {
        std::vector<telemetry::Phantom> phantoms;
        phantom_buffer_->snapshot_and_expire(phantoms, params_.phantom_ttl, zg::app::mono_now());
        phantom_positions.reserve(phantoms.size());
        for (const auto& p : phantoms) phantom_positions.push_back(p.position);
    }

    const bool active = added || !phantom_positions.empty();

    // Frozen and nothing perturbing it → skip the expensive integrate entirely.
    if (frozen && !active) {
        return StepResult{/*integrated=*/false, /*active=*/false};
    }

    // --- expensive work, only when awake ---

    // Snapshot the tombstone set into a mask sized to positions_. Stays empty
    // (zero allocation) in the common case of nothing deleted.
    std::vector<char> disabled;
    {
        std::lock_guard<std::mutex> lock(disabled_mu_);
        if (!disabled_.empty()) {
            disabled.assign(positions_.size(), 0);
            for (std::size_t idx : disabled_) {
                if (idx < disabled.size()) disabled[idx] = 1;
            }
        }
    }

    // Pull the runtime toggle through to SimParams so the integrator picks
    // up the latest setting without a restart.
    SimParams effective    = params_;
    effective.use_barnes_hut = use_barnes_hut_.load();
    integrate_step(positions_, velocities_, edges_, effective, phantom_positions, disabled);

    // Restore pinned nodes after integration so they appear physics-immune.
    // Done under pins_mu_ so set_pin / clear_pin from main can update the
    // anchor concurrently with a tick in progress without tearing.
    {
        std::lock_guard<std::mutex> lock(pins_mu_);
        for (const auto& [idx, anchor] : pins_) {
            if (idx < positions_.size()) {
                positions_[idx]  = anchor;
                velocities_[idx] = {0.0f, 0.0f, 0.0f};
            }
        }
    }

    return StepResult{/*integrated=*/true, active};
}

}  // namespace zg::physics
