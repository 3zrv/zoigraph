#pragma once

#include <raylib.h>

#include <string>
#include <vector>

namespace zg::telemetry {

// A "Phantom Node" injected from an external source over UDP. Distinct from
// the persistent Static Nodes that live in the SQLite graph: phantoms are
// ephemeral (TTL-bounded), additive-blended, and exist purely in memory until
// click-to-pin promotes them.
struct Phantom {
    long long              id;           // sender-assigned correlation id
    Vector3                position;
    std::string            label;        // optional human-readable tag for the inspector
    double                 spawn_time;   // GetTime() seconds at spawn, populated by the listener
    std::vector<long long> connections;  // optional Static Node ids this phantom is wired to
};

}  // namespace zg::telemetry
