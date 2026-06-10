#pragma once

#include <raylib.h>

#include <string>
#include <vector>

namespace zg::telemetry {

// One proposed connection from a phantom to a Static Node. `kind` is the
// relationship type the emitter assigned (one of zoigraph's edge kinds:
// "knows" / "owns" / "saw-at" / "shell-of" / "part-of" / "works-at" /
// "created" / "influenced" / ... or "" if the emitter didn't specify).
// On click-to-pin, `kind` lands on the materialised edge alongside the
// existing certainty="phantom" trust-gradient mark.
struct Connection {
    long long   target;
    std::string kind;
};

// A "Phantom Node" injected from an external source over UDP. Distinct from
// the persistent Static Nodes that live in the SQLite graph: phantoms are
// ephemeral (TTL-bounded), additive-blended, and exist purely in memory until
// click-to-pin promotes them.
struct Phantom {
    long long                id;           // sender-assigned correlation id
    Vector3                  position;
    std::string              label;        // optional human-readable tag for the inspector
    double                   spawn_time;   // GetTime() seconds at spawn, populated by the listener
    std::vector<Connection>  connections;  // optional Static Node connections (id + kind) this phantom is wired to
    std::string              source;       // optional emitter tag (e.g. "ollama:llama3.2:3b", "claude:sonnet"); empty if unspecified
    std::string              content;      // optional one-sentence reasoning from the emitter; lands as the node's markdown body on pin
    std::string              project;      // optional owning-project tag; the render loop drops phantoms tagged for another project (empty = untagged, always accepted)
    std::string              category;     // optional operator-facing grouping tag; the CLI /filter command hides phantoms whose category doesn't match (empty = uncategorized)
};

}  // namespace zg::telemetry
