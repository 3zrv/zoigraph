#pragma once

#include <raylib.h>

#include <string>
#include <vector>

#include "graph/types.h"

struct sqlite3;

namespace zg::persistence {

struct StoredNode {
    long long                id;
    Vector3                  position;
    std::string              title;
    std::string              content;
    double                   first_seen   = 0.0;          // Unix seconds; 0 == unknown
    double                   last_touched = 0.0;          // Unix seconds; bumped on edits
    std::string              tier         = "confirmed";  // "confirmed" / "suspected" / "phantom" / "self"
    std::vector<std::string> tags         = {};           // operator-extensible: subject / asset / hostile / ...
    bool                     deleted      = false;        // soft-delete tombstone; renderer + inspector skip these
};

// The app addresses nodes by their position in the loaded vector — edges store
// source/target as indices, not ids (the "id == vector index" model). Database
// itself is id-agnostic (load_graph round-trips arbitrary ids for storage
// tests), so the app verifies the invariant at project-open time. Returns the
// first index `i` where `nodes[i].id != i`, or nodes.size() when every id is
// contiguous 0..N-1. Pure; doctest-covered.
std::size_t first_noncontiguous_id(const std::vector<StoredNode>& nodes);

// Thin wrapper around a SQLite connection. The schema is the eventual home
// for everything that needs to survive a process restart: node positions,
// titles, markdown content, edges. SQLCipher will swap in transparently when
// we move past the milestone-1 plaintext build — the symbol surface here is
// identical to sqlite3's.
class Database {
public:
    // Opens (or creates) the file at `path`. Use ":memory:" for an ephemeral
    // DB. Throws std::runtime_error on any failure.
    explicit Database(const std::string& path);
    ~Database();

    Database(const Database&)            = delete;
    Database& operator=(const Database&) = delete;

    // Atomically replaces the stored graph with the given nodes and edges.
    void save_graph(const std::vector<StoredNode>& nodes,
                    const std::vector<graph::Edge>& edges);

    // Reads the entire graph. Returns false if the nodes table is empty.
    bool load_graph(std::vector<StoredNode>& nodes,
                    std::vector<graph::Edge>& edges) const;

    // Incremental single-row writes for the per-click hot paths: pin,
    // create-node, journal, soft-delete. save_graph stays the bulk path
    // (shutdown / project switch / explicit save button); these avoid
    // rewriting the whole DB + rebuilding FTS on every interaction. The
    // FTS index follows via the kSchema triggers.
    void insert_node(const StoredNode& n);     // node row + its tags, atomically
    void insert_edge(const graph::Edge& e);    // one edge row
    void mark_deleted(long long id);           // sets the tombstone flag

    // FTS5 prefix-match search across node titles + content. Returns matching
    // node ids ranked by FTS relevance, capped at a small limit. Empty query
    // (or one that sanitizes to nothing) returns no results.
    std::vector<long long> search(const std::string& query) const;

    // Updates the title and content of one node and bumps last_touched.
    // The FTS index is kept in sync via triggers, so a subsequent search()
    // sees the new text without any separate rebuild.
    void update_node_text(long long id, const std::string& title,
                          const std::string& content, double last_touched);

    // Updates only the tier field on one node. No FTS impact.
    void update_node_tier(long long id, const std::string& tier);

    // Replaces the tag set for one node atomically: deletes every existing
    // node_tags row for `id` and inserts the new set. No-op if the id
    // doesn't exist (no node + no tags).
    void update_node_tags(long long id, const std::vector<std::string>& tags);

    // Generic key-value metadata for project-wide settings (last open
    // timestamp, etc.). meta_double returns `fallback` if the key is
    // missing or fails to parse as a double; set_meta_double upserts.
    double meta_double(const std::string& key, double fallback) const;
    void   set_meta_double(const std::string& key, double value);

    // Updates label/kind/certainty on the single edge identified by
    // (source, target). Silently does nothing if no matching edge exists.
    // No-op for callers passing strings that don't change anything.
    void update_edge(std::size_t source, std::size_t target,
                     const std::string& label,
                     const std::string& kind,
                     const std::string& certainty);

    // Append-only telemetry log for the phase-2 trust-gradient measurement.
    // `kind` is a short tag identifying the event type
    // ("phantom_spawn", "phantom_pin", "phantom_decay", "node_edit",
    // "bones_throw"). `node_id` is the relevant node id if applicable
    // (-1 otherwise). `payload` is a free-form JSON blob whose schema
    // varies by kind. `ts` is captured automatically with sqlite3 julianday
    // converted to unix seconds at the SQL level. Single INSERT; safe to
    // call from the main thread at any frame rate.
    void log_event(const std::string& kind,
                   long long node_id,
                   const std::string& payload);

private:
    void exec(const char* sql);

    sqlite3* db_;
};

}  // namespace zg::persistence
