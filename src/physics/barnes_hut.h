#pragma once

#include <raylib.h>

#include <vector>

namespace zg::physics {

// Accumulates pairwise Coulomb repulsion onto `forces_out` using a Barnes-Hut
// octree approximation in O(N log N) instead of the naive O(N^2).
//
// Each particle carries unit charge. `theta` is the Barnes-Hut opening angle:
//   theta = 0     reduces to exact pairwise summation (slow; equivalent to
//                 the naive loop modulo floating-point order).
//   theta = 0.7   conventional default, good speed/accuracy trade.
//   theta > 1     visibly approximate but very fast.
//
// `forces_out` is added to, not overwritten — caller pre-sizes to match
// `positions` and may have other force contributions already accumulated
// (Hooke, centering, phantom repulsion).
//
// `disabled` (empty == none) masks out tombstoned nodes: a disabled node is
// kept out of the tree (it exerts no repulsion) and receives no force. Indices
// past the mask's end are treated as enabled.
void apply_barnes_hut_repulsion(const std::vector<Vector3>& positions,
                                std::vector<Vector3>& forces_out,
                                float repulsion_k,
                                float theta,
                                const std::vector<char>& disabled = {});

}  // namespace zg::physics
