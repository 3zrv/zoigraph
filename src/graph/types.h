#pragma once

#include <raylib.h>

#include <cstddef>
#include <string>

namespace zg::graph {

// Edge metadata is carried alongside the source/target indices so the
// physics integrator and the renderer/persistence layers all see the same
// struct. Physics ignores label/kind/certainty; they're string fields with
// sensible defaults so existing test cases that build edges as `{a, b}`
// continue to compile.
struct Edge {
    std::size_t source;
    std::size_t target;
    std::string label     = "";            // free-text label, optional
    std::string kind      = "";            // "" / "knows" / "owns" / "saw-at" / "shell-of" / ...
    std::string certainty = "confirmed";   // "confirmed" / "suspected" / "hearsay" / "phantom"
};

}  // namespace zg::graph
