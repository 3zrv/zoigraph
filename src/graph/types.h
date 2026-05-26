#pragma once

#include <raylib.h>

#include <cstddef>

namespace zg::graph {

struct Edge {
    std::size_t source;
    std::size_t target;
};

}  // namespace zg::graph
