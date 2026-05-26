#pragma once

#include <raylib.h>

#include <string>
#include <vector>

#include "graph/types.h"

struct sqlite3;

namespace zg::persistence {

struct StoredNode {
    long long   id;
    Vector3     position;
    std::string title;
    std::string content;
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

private:
    void exec(const char* sql);

    sqlite3* db_;
};

}  // namespace zg::persistence
