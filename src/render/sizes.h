#pragma once

namespace zg::render {

// World-space radii for the two kinds of nodes. Shared because both the
// raypick (collision spheres) and the renderer (halo wireframes) must
// agree on size — drift between them would mean clicking visibly-on-
// node space misses the pick.
constexpr float kNodeRadius    = 0.5f;
constexpr float kPhantomRadius = 1.2f;  // visibly larger than static nodes

}  // namespace zg::render
