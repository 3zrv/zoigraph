#include "persistence/db.h"

#include <sqlite3.h>

#include <cctype>
#include <stdexcept>
#include <string>

namespace zg::persistence {

namespace {

// Schema + FTS5 sync triggers. Per the SQLite FTS5 external-content recipe:
// the triggers keep nodes_fts in lockstep with the nodes table so any future
// INSERT / UPDATE / DELETE through SQL is reflected in the search index.
// A rebuild runs separately on connection open to catch rows that predate
// the triggers (e.g. DBs created before this code path existed).
constexpr const char* kSchema = R"SQL(
CREATE TABLE IF NOT EXISTS nodes (
    id      INTEGER PRIMARY KEY,
    x       REAL    NOT NULL,
    y       REAL    NOT NULL,
    z       REAL    NOT NULL,
    title   TEXT    NOT NULL DEFAULT '',
    content TEXT    NOT NULL DEFAULT ''
);
CREATE TABLE IF NOT EXISTS edges (
    source INTEGER NOT NULL,
    target INTEGER NOT NULL,
    weight REAL    NOT NULL DEFAULT 1.0
);
CREATE VIRTUAL TABLE IF NOT EXISTS nodes_fts USING fts5(
    title,
    content,
    content='nodes',
    content_rowid='id'
);
CREATE TRIGGER IF NOT EXISTS nodes_ai AFTER INSERT ON nodes BEGIN
    INSERT INTO nodes_fts(rowid, title, content)
        VALUES (new.id, new.title, new.content);
END;
CREATE TRIGGER IF NOT EXISTS nodes_ad AFTER DELETE ON nodes BEGIN
    INSERT INTO nodes_fts(nodes_fts, rowid, title, content)
        VALUES ('delete', old.id, old.title, old.content);
END;
CREATE TRIGGER IF NOT EXISTS nodes_au AFTER UPDATE ON nodes BEGIN
    INSERT INTO nodes_fts(nodes_fts, rowid, title, content)
        VALUES ('delete', old.id, old.title, old.content);
    INSERT INTO nodes_fts(rowid, title, content)
        VALUES (new.id, new.title, new.content);
END;
)SQL";

// Turns user input into an FTS5 prefix-match query, dropping any character
// that isn't alphanumeric so we don't have to escape FTS5 operator syntax.
// "foo bar" -> "foo* bar*".  Returns empty if nothing survives sanitization.
std::string to_fts_query(const std::string& q) {
    std::string out;
    bool in_token = false;
    for (char c : q) {
        if (std::isalnum(static_cast<unsigned char>(c))) {
            out += c;
            in_token = true;
        } else if (in_token) {
            out += "* ";
            in_token = false;
        }
    }
    if (in_token) out += "*";
    return out;
}

void throw_sqlite(sqlite3* db, const std::string& context) {
    std::string msg = context + ": " + (db ? sqlite3_errmsg(db) : "(no db)");
    throw std::runtime_error(msg);
}

}  // namespace

Database::Database(const std::string& path) : db_(nullptr) {
    if (sqlite3_open(path.c_str(), &db_) != SQLITE_OK) {
        const std::string msg = std::string("sqlite3_open failed: ") + sqlite3_errmsg(db_);
        sqlite3_close(db_);
        db_ = nullptr;
        throw std::runtime_error(msg);
    }
    exec(kSchema);
    // Catch up the FTS index against the current nodes table. No-op if the
    // triggers have been keeping it consistent; mandatory after a schema
    // upgrade from a pre-FTS5 build.
    exec("INSERT INTO nodes_fts(nodes_fts) VALUES('rebuild');");
}

Database::~Database() {
    if (db_) sqlite3_close(db_);
}

void Database::exec(const char* sql) {
    char* err = nullptr;
    if (sqlite3_exec(db_, sql, nullptr, nullptr, &err) != SQLITE_OK) {
        const std::string msg = std::string("sqlite3_exec failed: ") + (err ? err : "(null)");
        sqlite3_free(err);
        throw std::runtime_error(msg);
    }
}

void Database::save_graph(const std::vector<StoredNode>& nodes,
                          const std::vector<graph::Edge>& edges) {
    exec("BEGIN IMMEDIATE;");
    try {
        exec("DELETE FROM edges; DELETE FROM nodes;");

        sqlite3_stmt* ins_node = nullptr;
        if (sqlite3_prepare_v2(db_,
                "INSERT INTO nodes (id, x, y, z, title, content) VALUES (?,?,?,?,?,?);",
                -1, &ins_node, nullptr) != SQLITE_OK) {
            throw_sqlite(db_, "prepare INSERT nodes");
        }
        for (const StoredNode& n : nodes) {
            sqlite3_bind_int64(ins_node, 1, n.id);
            sqlite3_bind_double(ins_node, 2, n.position.x);
            sqlite3_bind_double(ins_node, 3, n.position.y);
            sqlite3_bind_double(ins_node, 4, n.position.z);
            sqlite3_bind_text(ins_node, 5, n.title.c_str(),   -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(ins_node, 6, n.content.c_str(), -1, SQLITE_TRANSIENT);
            if (sqlite3_step(ins_node) != SQLITE_DONE) {
                sqlite3_finalize(ins_node);
                throw_sqlite(db_, "step INSERT nodes");
            }
            sqlite3_reset(ins_node);
        }
        sqlite3_finalize(ins_node);

        sqlite3_stmt* ins_edge = nullptr;
        if (sqlite3_prepare_v2(db_,
                "INSERT INTO edges (source, target, weight) VALUES (?,?,?);",
                -1, &ins_edge, nullptr) != SQLITE_OK) {
            throw_sqlite(db_, "prepare INSERT edges");
        }
        for (const graph::Edge& e : edges) {
            sqlite3_bind_int64(ins_edge, 1, static_cast<long long>(e.source));
            sqlite3_bind_int64(ins_edge, 2, static_cast<long long>(e.target));
            sqlite3_bind_double(ins_edge, 3, 1.0);
            if (sqlite3_step(ins_edge) != SQLITE_DONE) {
                sqlite3_finalize(ins_edge);
                throw_sqlite(db_, "step INSERT edges");
            }
            sqlite3_reset(ins_edge);
        }
        sqlite3_finalize(ins_edge);

        // Rebuild the FTS index from the freshly-populated nodes table so
        // search results never reference stale rows.
        exec("INSERT INTO nodes_fts(nodes_fts) VALUES('rebuild');");

        exec("COMMIT;");
    } catch (...) {
        exec("ROLLBACK;");
        throw;
    }
}

bool Database::load_graph(std::vector<StoredNode>& nodes,
                          std::vector<graph::Edge>& edges) const {
    nodes.clear();
    edges.clear();

    sqlite3_stmt* q_nodes = nullptr;
    if (sqlite3_prepare_v2(db_,
            "SELECT id, x, y, z, title, content FROM nodes ORDER BY id;",
            -1, &q_nodes, nullptr) != SQLITE_OK) {
        throw_sqlite(db_, "prepare SELECT nodes");
    }
    while (sqlite3_step(q_nodes) == SQLITE_ROW) {
        StoredNode n{};
        n.id         = sqlite3_column_int64(q_nodes, 0);
        n.position.x = static_cast<float>(sqlite3_column_double(q_nodes, 1));
        n.position.y = static_cast<float>(sqlite3_column_double(q_nodes, 2));
        n.position.z = static_cast<float>(sqlite3_column_double(q_nodes, 3));
        if (const unsigned char* t = sqlite3_column_text(q_nodes, 4)) n.title   = reinterpret_cast<const char*>(t);
        if (const unsigned char* c = sqlite3_column_text(q_nodes, 5)) n.content = reinterpret_cast<const char*>(c);
        nodes.push_back(std::move(n));
    }
    sqlite3_finalize(q_nodes);

    sqlite3_stmt* q_edges = nullptr;
    if (sqlite3_prepare_v2(db_,
            "SELECT source, target FROM edges;",
            -1, &q_edges, nullptr) != SQLITE_OK) {
        throw_sqlite(db_, "prepare SELECT edges");
    }
    while (sqlite3_step(q_edges) == SQLITE_ROW) {
        graph::Edge e{};
        e.source = static_cast<std::size_t>(sqlite3_column_int64(q_edges, 0));
        e.target = static_cast<std::size_t>(sqlite3_column_int64(q_edges, 1));
        edges.push_back(e);
    }
    sqlite3_finalize(q_edges);

    return !nodes.empty();
}

void Database::update_node_text(long long id, const std::string& title, const std::string& content) {
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_,
            "UPDATE nodes SET title = ?, content = ? WHERE id = ?;",
            -1, &stmt, nullptr) != SQLITE_OK) {
        throw_sqlite(db_, "prepare UPDATE nodes");
    }
    sqlite3_bind_text (stmt, 1, title.c_str(),   -1, SQLITE_TRANSIENT);
    sqlite3_bind_text (stmt, 2, content.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 3, id);
    if (sqlite3_step(stmt) != SQLITE_DONE) {
        sqlite3_finalize(stmt);
        throw_sqlite(db_, "step UPDATE nodes");
    }
    sqlite3_finalize(stmt);
}

std::vector<long long> Database::search(const std::string& query) const {
    const std::string fts = to_fts_query(query);
    if (fts.empty()) return {};

    std::vector<long long> ids;
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_,
            "SELECT rowid FROM nodes_fts WHERE nodes_fts MATCH ? ORDER BY rank LIMIT 50;",
            -1, &stmt, nullptr) != SQLITE_OK) {
        throw_sqlite(db_, "prepare search");
    }
    sqlite3_bind_text(stmt, 1, fts.c_str(), -1, SQLITE_TRANSIENT);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        ids.push_back(sqlite3_column_int64(stmt, 0));
    }
    sqlite3_finalize(stmt);
    return ids;
}

}  // namespace zg::persistence
