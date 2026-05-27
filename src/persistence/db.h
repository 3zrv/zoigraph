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
};

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

private:
    void exec(const char* sql);

    sqlite3* db_;
};

}  // namespace zg::persistence
