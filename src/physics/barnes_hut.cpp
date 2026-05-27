#include "physics/barnes_hut.h"

#include <algorithm>
#include <cmath>

namespace zg::physics {
namespace {

constexpr float kMinDistance = 0.01f;
constexpr int   kMaxDepth    = 30;  // backstop for near-coincident particles

// Array-backed octree. Children are stored contiguously in groups of 8 right
// after their parent in the same vector; child_base == -1 marks a leaf.
struct OctreeNode {
    Vector3 center{};            // center of this cell's AABB
    float   half_size = 0.0f;    // half side length
    Vector3 com{};               // center of mass (unweighted sum / count)
    float   total_mass = 0.0f;   // count of particles in this subtree
    int     child_base = -1;     // index of first of 8 children, or -1
    int     particle_idx = -1;   // valid only if leaf and occupied
    bool    has_particle = false;
};

int octant_of(Vector3 p, Vector3 c) {
    int o = 0;
    if (p.x >= c.x) o |= 1;
    if (p.y >= c.y) o |= 2;
    if (p.z >= c.z) o |= 4;
    return o;
}

Vector3 child_center(Vector3 c, float half, int octant) {
    const float q = half * 0.5f;
    return {
        c.x + ((octant & 1) ? q : -q),
        c.y + ((octant & 2) ? q : -q),
        c.z + ((octant & 4) ? q : -q),
    };
}

void insert(std::vector<OctreeNode>& tree,
            int node_idx,
            int particle_idx,
            const std::vector<Vector3>& positions,
            int depth) {
    if (depth > kMaxDepth) return;  // near-coincident particles: drop excess

    // Empty leaf — take it.
    if (tree[node_idx].child_base == -1 && !tree[node_idx].has_particle) {
        tree[node_idx].particle_idx = particle_idx;
        tree[node_idx].has_particle = true;
        return;
    }

    // Occupied leaf — subdivide, evict the held particle, then drop through
    // to the internal-node case.
    if (tree[node_idx].child_base == -1) {
        const Vector3 c    = tree[node_idx].center;
        const float   half = tree[node_idx].half_size;
        const int     held = tree[node_idx].particle_idx;
        const int     base = static_cast<int>(tree.size());

        for (int i = 0; i < 8; ++i) {
            OctreeNode child{};
            child.center    = child_center(c, half, i);
            child.half_size = half * 0.5f;
            tree.push_back(child);
        }
        // References into `tree` may have been invalidated by push_back; use
        // index lookups from here on.
        tree[node_idx].child_base   = base;
        tree[node_idx].has_particle = false;
        tree[node_idx].particle_idx = -1;

        insert(tree,
               base + octant_of(positions[held], tree[node_idx].center),
               held,
               positions,
               depth + 1);
    }

    // Internal — descend.
    const int base = tree[node_idx].child_base;
    insert(tree,
           base + octant_of(positions[particle_idx], tree[node_idx].center),
           particle_idx,
           positions,
           depth + 1);
}

void compute_com(std::vector<OctreeNode>& tree, int node_idx,
                 const std::vector<Vector3>& positions) {
    if (tree[node_idx].child_base == -1) {
        if (tree[node_idx].has_particle) {
            tree[node_idx].com        = positions[tree[node_idx].particle_idx];
            tree[node_idx].total_mass = 1.0f;
        } else {
            tree[node_idx].total_mass = 0.0f;
        }
        return;
    }
    Vector3 weighted{0, 0, 0};
    float   total = 0.0f;
    const int base = tree[node_idx].child_base;
    for (int i = 0; i < 8; ++i) {
        compute_com(tree, base + i, positions);
        const OctreeNode& c = tree[base + i];
        weighted.x += c.com.x * c.total_mass;
        weighted.y += c.com.y * c.total_mass;
        weighted.z += c.com.z * c.total_mass;
        total      += c.total_mass;
    }
    if (total > 0.0f) {
        tree[node_idx].com = {weighted.x / total, weighted.y / total, weighted.z / total};
    }
    tree[node_idx].total_mass = total;
}

Vector3 force_from_cell(const std::vector<OctreeNode>& tree, int node_idx,
                        Vector3 pos, int self_idx,
                        float repulsion_k, float theta) {
    const OctreeNode& n = tree[node_idx];
    if (n.total_mass <= 0.0f) return {0, 0, 0};

    // Leaf with a particle — direct Coulomb (skip self).
    if (n.child_base == -1) {
        if (!n.has_particle || n.particle_idx == self_idx) return {0, 0, 0};
        const Vector3 d{pos.x - n.com.x, pos.y - n.com.y, pos.z - n.com.z};
        float r2 = d.x*d.x + d.y*d.y + d.z*d.z;
        if (r2 < kMinDistance * kMinDistance) r2 = kMinDistance * kMinDistance;
        const float r   = std::sqrt(r2);
        const float mag = repulsion_k / r2;
        return {d.x / r * mag, d.y / r * mag, d.z / r * mag};
    }

    // Internal — Barnes-Hut opening criterion.
    const Vector3 d{pos.x - n.com.x, pos.y - n.com.y, pos.z - n.com.z};
    const float   dist     = std::sqrt(d.x*d.x + d.y*d.y + d.z*d.z);
    const float   cell_sz  = n.half_size * 2.0f;
    if (dist > 0.0f && (cell_sz / dist) < theta) {
        // Treat the entire cell as a single body at its center of mass.
        float r2 = dist * dist;
        if (r2 < kMinDistance * kMinDistance) r2 = kMinDistance * kMinDistance;
        const float r   = std::sqrt(r2);
        const float mag = repulsion_k * n.total_mass / r2;
        return {d.x / r * mag, d.y / r * mag, d.z / r * mag};
    }

    Vector3 sum{0, 0, 0};
    for (int i = 0; i < 8; ++i) {
        const Vector3 f = force_from_cell(tree, n.child_base + i, pos, self_idx,
                                          repulsion_k, theta);
        sum.x += f.x;
        sum.y += f.y;
        sum.z += f.z;
    }
    return sum;
}

}  // namespace

void apply_barnes_hut_repulsion(const std::vector<Vector3>& positions,
                                std::vector<Vector3>& forces_out,
                                float repulsion_k,
                                float theta) {
    const std::size_t n = positions.size();
    if (n < 2) return;

    // Bounding cube around every particle (a touch larger so points on the
    // boundary land cleanly inside).
    Vector3 lo = positions[0];
    Vector3 hi = positions[0];
    for (const auto& p : positions) {
        lo.x = std::min(lo.x, p.x); hi.x = std::max(hi.x, p.x);
        lo.y = std::min(lo.y, p.y); hi.y = std::max(hi.y, p.y);
        lo.z = std::min(lo.z, p.z); hi.z = std::max(hi.z, p.z);
    }
    const Vector3 center{
        (lo.x + hi.x) * 0.5f,
        (lo.y + hi.y) * 0.5f,
        (lo.z + hi.z) * 0.5f,
    };
    const float span = std::max({hi.x - lo.x, hi.y - lo.y, hi.z - lo.z});
    const float half = span * 0.5f + 1.0f;

    std::vector<OctreeNode> tree;
    tree.reserve(n * 4);  // rough heuristic for a balanced-ish tree

    OctreeNode root{};
    root.center    = center;
    root.half_size = half;
    tree.push_back(root);

    for (std::size_t i = 0; i < n; ++i) {
        insert(tree, 0, static_cast<int>(i), positions, 0);
    }
    compute_com(tree, 0, positions);

    for (std::size_t i = 0; i < n; ++i) {
        const Vector3 f = force_from_cell(tree, 0, positions[i],
                                          static_cast<int>(i),
                                          repulsion_k, theta);
        forces_out[i].x += f.x;
        forces_out[i].y += f.y;
        forces_out[i].z += f.z;
    }
}

}  // namespace zg::physics
