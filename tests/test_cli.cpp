#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "ui/cli_tab.h"

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "app/session.h"
#include "persistence/db.h"

using zg::ui::cli_complete;
using zg::ui::cli_history_push;
using zg::ui::cli_history_step;
using zg::ui::cli_tokenize;
using zg::ui::CliDeps;
using zg::ui::CliState;
using zg::ui::run_cli_command;

namespace {

// Most commands degrade gracefully on missing deps; tests that don't care
// about deps share one empty instance.
CliDeps null_deps;

bool any_line_contains(const std::vector<std::string>& sb,
                       const std::string& needle) {
    for (const auto& l : sb) {
        if (l.find(needle) != std::string::npos) return true;
    }
    return false;
}

// Minimal live session: in-memory DB, no physics thread (commands skip the
// enqueue when physics is null), a couple of pre-seeded nodes.
struct Fixture {
    zg::app::Session session;
    CliDeps          deps;

    Fixture() {
        session.db = std::make_unique<zg::persistence::Database>(":memory:");
        add_node("alpha");
        add_node("bravo site");
        deps.session = &session;
    }
    void add_node(const std::string& title) {
        zg::persistence::StoredNode n{};
        n.id    = static_cast<long long>(session.stored_nodes.size());
        n.title = title;
        session.stored_nodes.push_back(n);
        session.db->insert_node(session.stored_nodes.back());
    }
};

}  // namespace

// ---- tokenizer --------------------------------------------------------

TEST_CASE("tokenize: empty and whitespace-only input yields no tokens") {
    CHECK(cli_tokenize("").empty());
    CHECK(cli_tokenize("   \t ").empty());
}

TEST_CASE("tokenize: splits on runs of whitespace") {
    const auto t = cli_tokenize("  /node   alpha\tbeta ");
    REQUIRE(t.size() == 3);
    CHECK(t[0] == "/node");
    CHECK(t[1] == "alpha");
    CHECK(t[2] == "beta");
}

TEST_CASE("tokenize: double quotes keep spaces inside one token") {
    const auto t = cli_tokenize("/node \"safe house bravo\" --to 3");
    REQUIRE(t.size() == 4);
    CHECK(t[1] == "safe house bravo");
}

TEST_CASE("tokenize: quotes can glue mid-token") {
    const auto t = cli_tokenize("/node pre\"fix and\"post");
    REQUIRE(t.size() == 2);
    CHECK(t[1] == "prefix andpost");
}

TEST_CASE("tokenize: unterminated quote runs to end of line") {
    const auto t = cli_tokenize("/search \"operation midnight");
    REQUIRE(t.size() == 2);
    CHECK(t[1] == "operation midnight");
}

TEST_CASE("tokenize: empty quoted string is a real (empty) token") {
    const auto t = cli_tokenize("/node \"\"");
    REQUIRE(t.size() == 2);
    CHECK(t[1].empty());
}

// ---- history ----------------------------------------------------------

TEST_CASE("history: stepping in an empty history does nothing") {
    CliState cli;
    CHECK_FALSE(cli_history_step(cli, -1, "draft").has_value());
    CHECK_FALSE(cli_history_step(cli, +1, "draft").has_value());
}

TEST_CASE("history: up walks older, stops at the oldest") {
    CliState cli;
    cli_history_push(cli, "/info");
    cli_history_push(cli, "/projects");
    auto s = cli_history_step(cli, -1, "");
    REQUIRE(s.has_value());
    CHECK(*s == "/projects");
    s = cli_history_step(cli, -1, *s);
    REQUIRE(s.has_value());
    CHECK(*s == "/info");
    CHECK_FALSE(cli_history_step(cli, -1, *s).has_value());  // already oldest
}

TEST_CASE("history: down past the newest restores the stashed draft") {
    CliState cli;
    cli_history_push(cli, "/info");
    cli_history_step(cli, -1, "half-typed draft");
    auto s = cli_history_step(cli, +1, "/info");
    REQUIRE(s.has_value());
    CHECK(*s == "half-typed draft");
    // Back on the fresh line now; another Down is a no-op.
    CHECK_FALSE(cli_history_step(cli, +1, *s).has_value());
}

TEST_CASE("history: push skips blanks and dedupes consecutive repeats") {
    CliState cli;
    cli_history_push(cli, "");
    cli_history_push(cli, "   ");
    CHECK(cli.history.empty());
    cli_history_push(cli, "/info");
    cli_history_push(cli, "/info");
    CHECK(cli.history.size() == 1);
    cli_history_push(cli, "/projects");
    cli_history_push(cli, "/info");
    CHECK(cli.history.size() == 3);  // non-consecutive repeat is kept
}

TEST_CASE("history: push resets navigation state") {
    CliState cli;
    cli_history_push(cli, "/info");
    cli_history_step(cli, -1, "draft");
    cli_history_push(cli, "/projects");
    CHECK(cli.hist_pos == -1);
    // Fresh navigation starts from the newest entry again.
    auto s = cli_history_step(cli, -1, "");
    REQUIRE(s.has_value());
    CHECK(*s == "/projects");
}

// ---- completion -------------------------------------------------------

TEST_CASE("complete: unique prefix completes with a trailing space") {
    const auto c = cli_complete("/sea");
    CHECK(c.completed == "/search ");
    REQUIRE(c.candidates.size() == 1);
    CHECK(c.candidates[0] == "/search");
}

TEST_CASE("complete: ambiguous prefix extends to the common prefix") {
    const auto c = cli_complete("/se");
    CHECK(c.candidates.size() >= 3);  // /search /set /settings at minimum
    CHECK(c.completed == "/se");      // 'a' vs 't' diverge immediately
    const auto c2 = cli_complete("/set");
    CHECK(c2.candidates.size() == 2);  // /set /settings
    CHECK(c2.completed == "/set");
}

TEST_CASE("complete: no match, no slash, or past the command word: no-op") {
    CHECK(cli_complete("/zz").candidates.empty());
    CHECK(cli_complete("/zz").completed == "/zz");
    CHECK(cli_complete("help").candidates.empty());
    CHECK(cli_complete("/node \"ti").candidates.empty());
    CHECK(cli_complete("").candidates.empty());
}

TEST_CASE("complete: exact command still completes (adds the space)") {
    const auto c = cli_complete("/panic");
    REQUIRE(c.candidates.size() == 1);
    CHECK(c.completed == "/panic ");
}

// ---- dispatch ---------------------------------------------------------

TEST_CASE("cli: blank and whitespace-only lines are ignored") {
    std::vector<std::string> sb;
    CHECK_FALSE(run_cli_command("", null_deps, sb));
    CHECK_FALSE(run_cli_command("   \t ", null_deps, sb));
    CHECK(sb.empty());
}

TEST_CASE("cli: /panic fires and echoes what it is about to do") {
    std::vector<std::string> sb;
    CHECK(run_cli_command("/panic", null_deps, sb));
    REQUIRE(sb.size() == 2);
    CHECK(sb[0] == "> /panic");
    CHECK(sb[1].find("wiping") != std::string::npos);
}

TEST_CASE("cli: surrounding whitespace doesn't defeat a command") {
    std::vector<std::string> sb;
    CHECK(run_cli_command("  /panic \t", null_deps, sb));
}

TEST_CASE("cli: /clear empties the scrollback, echo included") {
    std::vector<std::string> sb = {"old line", "another"};
    CHECK_FALSE(run_cli_command("/clear", null_deps, sb));
    CHECK(sb.empty());
}

TEST_CASE("cli: /help lists /panic and never fires") {
    std::vector<std::string> sb;
    CHECK_FALSE(run_cli_command("/help", null_deps, sb));
    CHECK(any_line_contains(sb, "/panic"));
}

TEST_CASE("cli: unknown commands report themselves and never fire") {
    std::vector<std::string> sb;
    CHECK_FALSE(run_cli_command("/wat", null_deps, sb));
    REQUIRE(sb.size() == 2);
    CHECK(sb[0] == "> /wat");
    CHECK(sb[1].find("unknown command: /wat") != std::string::npos);
}

TEST_CASE("cli: a near-miss of /panic does not fire") {
    std::vector<std::string> sb;
    CHECK_FALSE(run_cli_command("/panic now", null_deps, sb));
    CHECK(any_line_contains(sb, "takes no arguments"));
    CHECK_FALSE(run_cli_command("panic", null_deps, sb));
}

// ---- /node + /edge ----------------------------------------------------

TEST_CASE("cli: /node without a project reports instead of crashing") {
    std::vector<std::string> sb;
    CHECK_FALSE(run_cli_command("/node x", null_deps, sb));
    CHECK(any_line_contains(sb, "no active project"));
}

TEST_CASE("cli: /node creates a stored node with the default tier") {
    Fixture f;
    std::vector<std::string> sb;
    run_cli_command("/node charlie", f.deps, sb);
    REQUIRE(f.session.stored_nodes.size() == 3);
    CHECK(f.session.stored_nodes[2].title == "charlie");
    CHECK(f.session.stored_nodes[2].tier == "confirmed");
    CHECK(f.session.edges.empty());
}

TEST_CASE("cli: /node with quoted title and tier") {
    Fixture f;
    std::vector<std::string> sb;
    run_cli_command("/node \"dead drop east\" --tier suspected", f.deps, sb);
    REQUIRE(f.session.stored_nodes.size() == 3);
    CHECK(f.session.stored_nodes[2].title == "dead drop east");
    CHECK(f.session.stored_nodes[2].tier == "suspected");
}

TEST_CASE("cli: /node rejects a bogus tier") {
    Fixture f;
    std::vector<std::string> sb;
    run_cli_command("/node x --tier banana", f.deps, sb);
    CHECK(f.session.stored_nodes.size() == 2);  // nothing created
    CHECK(any_line_contains(sb, "tier must be"));
}

TEST_CASE("cli: /node --to creates the edge, resolving by id") {
    Fixture f;
    std::vector<std::string> sb;
    run_cli_command("/node charlie --to 0", f.deps, sb);
    REQUIRE(f.session.edges.size() == 1);
    CHECK(f.session.edges[0].source == 2);
    CHECK(f.session.edges[0].target == 0);
    CHECK(f.session.edges[0].certainty == "confirmed");
}

TEST_CASE("cli: /node --to resolves by exact title including spaces") {
    Fixture f;
    std::vector<std::string> sb;
    run_cli_command("/node charlie --to \"bravo site\"", f.deps, sb);
    REQUIRE(f.session.edges.size() == 1);
    CHECK(f.session.edges[0].target == 1);
}

TEST_CASE("cli: /node --to resolves by FTS when no exact title matches") {
    Fixture f;
    std::vector<std::string> sb;
    run_cli_command("/node charlie --to bravo", f.deps, sb);
    REQUIRE(f.session.edges.size() == 1);
    CHECK(f.session.edges[0].target == 1);
}

TEST_CASE("cli: /node --to with an unresolvable ref creates nothing") {
    Fixture f;
    std::vector<std::string> sb;
    run_cli_command("/node charlie --to zulu", f.deps, sb);
    CHECK(f.session.stored_nodes.size() == 2);
    CHECK(f.session.edges.empty());
    CHECK(any_line_contains(sb, "no node matching"));
}

TEST_CASE("cli: /node --to never resolves a tombstoned node") {
    Fixture f;
    f.session.stored_nodes[1].deleted = true;
    std::vector<std::string> sb;
    run_cli_command("/node charlie --to \"bravo site\"", f.deps, sb);
    CHECK(f.session.edges.empty());
    CHECK(any_line_contains(sb, "no node matching"));
}

TEST_CASE("cli: /edge connects two existing nodes with an optional kind") {
    Fixture f;
    std::vector<std::string> sb;
    run_cli_command("/edge alpha \"bravo site\" knows", f.deps, sb);
    REQUIRE(f.session.edges.size() == 1);
    CHECK(f.session.edges[0].source == 0);
    CHECK(f.session.edges[0].target == 1);
    CHECK(f.session.edges[0].kind == "knows");
}

TEST_CASE("cli: /edge refuses a self-edge") {
    Fixture f;
    std::vector<std::string> sb;
    run_cli_command("/edge alpha alpha", f.deps, sb);
    CHECK(f.session.edges.empty());
    CHECK(any_line_contains(sb, "itself"));
}

TEST_CASE("cli: /edge with a numeric ref out of range fails cleanly") {
    Fixture f;
    std::vector<std::string> sb;
    run_cli_command("/edge 0 99", f.deps, sb);
    CHECK(f.session.edges.empty());
    CHECK(any_line_contains(sb, "no node matching '99'"));
}

// ---- /search ------------------------------------------------------------

TEST_CASE("cli: /search lists hits and selects the top one") {
    Fixture f;
    f.session.positions = {Vector3{0, 0, 0}, Vector3{5, 0, 5}};
    std::vector<std::string> sb;
    run_cli_command("/search bravo", f.deps, sb);
    CHECK(any_line_contains(sb, "1 match"));
    CHECK(any_line_contains(sb, "bravo site"));
    CHECK(f.session.selected_node == 1);
    CHECK(f.session.search_query == "bravo");
    REQUIRE(f.session.search_hits.size() == 1);
    CHECK(f.session.search_hits[0] == 1);
}

TEST_CASE("cli: /search joins multi-word queries") {
    Fixture f;
    std::vector<std::string> sb;
    run_cli_command("/search bravo site", f.deps, sb);
    CHECK(f.session.search_query == "bravo site");
}

TEST_CASE("cli: /search with no matches says so") {
    Fixture f;
    std::vector<std::string> sb;
    run_cli_command("/search zulu", f.deps, sb);
    CHECK(any_line_contains(sb, "no matches"));
    CHECK(f.session.selected_node == -1);
}

TEST_CASE("cli: /search without a query prints usage") {
    Fixture f;
    std::vector<std::string> sb;
    run_cli_command("/search", f.deps, sb);
    CHECK(any_line_contains(sb, "usage:"));
}

// ---- /select + /neighbors ------------------------------------------------

TEST_CASE("cli: /select resolves a ref and selects it") {
    Fixture f;
    f.session.positions = {Vector3{0, 0, 0}, Vector3{5, 0, 5}};
    std::vector<std::string> sb;
    run_cli_command("/select \"bravo site\"", f.deps, sb);
    CHECK(f.session.selected_node == 1);
    CHECK(any_line_contains(sb, "bravo site"));

    run_cli_command("/select 0", f.deps, sb);
    CHECK(f.session.selected_node == 0);
}

TEST_CASE("cli: /select with an unresolvable ref changes nothing") {
    Fixture f;
    f.session.selected_node = 1;
    std::vector<std::string> sb;
    run_cli_command("/select zulu", f.deps, sb);
    CHECK(f.session.selected_node == 1);
    CHECK(any_line_contains(sb, "no node matching"));
}

TEST_CASE("cli: /select without args prints usage") {
    Fixture f;
    std::vector<std::string> sb;
    run_cli_command("/select", f.deps, sb);
    CHECK(any_line_contains(sb, "usage:"));
}

TEST_CASE("cli: /neighbors lists incident edges in both directions") {
    Fixture f;
    f.add_node("charlie");
    f.session.edges.push_back({0, 1, "", "knows", "confirmed"});
    f.session.edges.push_back({2, 0, "", "", "suspected"});
    std::vector<std::string> sb;
    run_cli_command("/neighbors alpha", f.deps, sb);
    CHECK(any_line_contains(sb, "bravo site"));
    CHECK(any_line_contains(sb, "charlie"));
    CHECK(any_line_contains(sb, "knows"));
    CHECK(any_line_contains(sb, "2 edges"));
}

TEST_CASE("cli: /neighbors skips edges to tombstoned nodes") {
    Fixture f;
    f.session.edges.push_back({0, 1, "", "", "confirmed"});
    f.session.stored_nodes[1].deleted = true;
    std::vector<std::string> sb;
    run_cli_command("/neighbors alpha", f.deps, sb);
    CHECK_FALSE(any_line_contains(sb, "bravo site"));
    CHECK(any_line_contains(sb, "no edges"));
}

TEST_CASE("cli: /neighbors on an isolated node says so") {
    Fixture f;
    std::vector<std::string> sb;
    run_cli_command("/neighbors alpha", f.deps, sb);
    CHECK(any_line_contains(sb, "no edges"));
}

// ---- /tier + /delete -----------------------------------------------------

TEST_CASE("cli: /tier changes a node's tier") {
    Fixture f;
    std::vector<std::string> sb;
    run_cli_command("/tier alpha suspected", f.deps, sb);
    CHECK(f.session.stored_nodes[0].tier == "suspected");
    CHECK(any_line_contains(sb, "alpha"));
}

TEST_CASE("cli: /tier rejects bogus tiers and the self node") {
    Fixture f;
    std::vector<std::string> sb;
    run_cli_command("/tier alpha banana", f.deps, sb);
    CHECK(f.session.stored_nodes[0].tier == "confirmed");
    CHECK(any_line_contains(sb, "tier must be"));

    f.session.self_idx = 0;
    run_cli_command("/tier alpha suspected", f.deps, sb);
    CHECK(f.session.stored_nodes[0].tier == "confirmed");
    CHECK(any_line_contains(sb, "self node"));
}

TEST_CASE("cli: /delete without --confirm arms but deletes nothing") {
    Fixture f;
    std::vector<std::string> sb;
    run_cli_command("/delete \"bravo site\"", f.deps, sb);
    CHECK_FALSE(f.session.stored_nodes[1].deleted);
    // The confirmation instruction names the RESOLVED id, so what the
    // operator confirms is exactly what was resolved, not a re-resolution.
    CHECK(any_line_contains(sb, "/delete 1 --confirm"));
}

TEST_CASE("cli: /delete --confirm tombstones and deselects") {
    Fixture f;
    f.session.selected_node = 1;
    std::vector<std::string> sb;
    run_cli_command("/delete 1 --confirm", f.deps, sb);
    CHECK(f.session.stored_nodes[1].deleted);
    CHECK(f.session.selected_node == -1);
    CHECK(any_line_contains(sb, "deleted"));
    // Tombstoned now: a second resolve must fail.
    sb.clear();
    run_cli_command("/delete 1 --confirm", f.deps, sb);
    CHECK(any_line_contains(sb, "no node matching"));
}

TEST_CASE("cli: /delete keeps an unrelated selection") {
    Fixture f;
    f.session.selected_node = 0;
    std::vector<std::string> sb;
    run_cli_command("/delete 1 --confirm", f.deps, sb);
    CHECK(f.session.selected_node == 0);
}

TEST_CASE("cli: /delete refuses the self node") {
    Fixture f;
    f.session.self_idx = 0;
    std::vector<std::string> sb;
    run_cli_command("/delete alpha --confirm", f.deps, sb);
    CHECK_FALSE(f.session.stored_nodes[0].deleted);
    CHECK(any_line_contains(sb, "self node"));
}

// ---- /ask -----------------------------------------------------------------

TEST_CASE("cli: /ask fires the hook with the resolved node") {
    Fixture f;
    std::vector<long long> asked;
    f.deps.ask_start = [&](long long id) { asked.push_back(id); };
    std::vector<std::string> sb;
    run_cli_command("/ask \"bravo site\"", f.deps, sb);
    REQUIRE(asked.size() == 1);
    CHECK(asked[0] == 1);
    CHECK(any_line_contains(sb, "asking about"));
}

TEST_CASE("cli: /ask without a ref uses the selection") {
    Fixture f;
    f.session.selected_node = 0;
    std::vector<long long> asked;
    f.deps.ask_start = [&](long long id) { asked.push_back(id); };
    std::vector<std::string> sb;
    run_cli_command("/ask", f.deps, sb);
    REQUIRE(asked.size() == 1);
    CHECK(asked[0] == 0);
}

TEST_CASE("cli: /ask with nothing selected and no ref says so") {
    Fixture f;
    std::vector<long long> asked;
    f.deps.ask_start = [&](long long id) { asked.push_back(id); };
    std::vector<std::string> sb;
    run_cli_command("/ask", f.deps, sb);
    CHECK(asked.empty());
    CHECK(any_line_contains(sb, "nothing selected"));
}

TEST_CASE("cli: /ask degrades politely without the hook or a match") {
    Fixture f;
    std::vector<std::string> sb;
    run_cli_command("/ask alpha", f.deps, sb);
    CHECK(any_line_contains(sb, "ask unavailable"));
    f.deps.ask_start = [](long long) {};
    sb.clear();
    run_cli_command("/ask zulu", f.deps, sb);
    CHECK(any_line_contains(sb, "no node matching"));
}

// ---- /projects, /project, /info ----------------------------------------

namespace {

// Adds a throwaway projects dir with fake .db files + an open_project stub
// that records what it was asked to open.
struct ProjectFixture : Fixture {
    std::filesystem::path    dir;
    std::vector<std::string> opened;

    ProjectFixture() {
        dir = std::filesystem::temp_directory_path() / "zg_cli_projects";
        std::filesystem::remove_all(dir);
        std::filesystem::create_directories(dir);
        for (const char* n : {"default", "ops"}) {
            std::ofstream(dir / (std::string(n) + ".db")) << "x";
        }
        session.current_project = "default";
        deps.projects_dir       = dir;
        deps.open_project = [this](const std::string& n) { opened.push_back(n); };
    }
    ~ProjectFixture() { std::filesystem::remove_all(dir); }
};

}  // namespace

TEST_CASE("cli: /projects lists projects and stars the active one") {
    ProjectFixture f;
    std::vector<std::string> sb;
    run_cli_command("/projects", f.deps, sb);
    CHECK(any_line_contains(sb, "* default"));
    CHECK(any_line_contains(sb, "  ops"));
}

TEST_CASE("cli: /project switches to an existing project") {
    ProjectFixture f;
    std::vector<std::string> sb;
    run_cli_command("/project ops", f.deps, sb);
    REQUIRE(f.opened.size() == 1);
    CHECK(f.opened[0] == "ops");
    CHECK(any_line_contains(sb, "switched to ops"));
}

TEST_CASE("cli: /project refuses to invent a project without --create") {
    ProjectFixture f;
    std::vector<std::string> sb;
    run_cli_command("/project nope", f.deps, sb);
    CHECK(f.opened.empty());
    CHECK(any_line_contains(sb, "no such project"));
}

TEST_CASE("cli: /project --create makes a new project") {
    ProjectFixture f;
    std::vector<std::string> sb;
    run_cli_command("/project fresh --create", f.deps, sb);
    REQUIRE(f.opened.size() == 1);
    CHECK(f.opened[0] == "fresh");
}

TEST_CASE("cli: /project validates the name") {
    ProjectFixture f;
    std::vector<std::string> sb;
    run_cli_command("/project \"../escape\" --create", f.deps, sb);
    CHECK(f.opened.empty());
    CHECK(any_line_contains(sb, "name must be"));
}

TEST_CASE("cli: /project is a no-op on the already-active project") {
    ProjectFixture f;
    std::vector<std::string> sb;
    run_cli_command("/project default", f.deps, sb);
    CHECK(f.opened.empty());
    CHECK(any_line_contains(sb, "already on default"));
}

TEST_CASE("cli: /info reports node/edge counts and tombstones") {
    ProjectFixture f;
    f.session.stored_nodes[1].deleted = true;
    std::vector<std::string> sb;
    run_cli_command("/info", f.deps, sb);
    CHECK(any_line_contains(sb, "project   default"));
    CHECK(any_line_contains(sb, "nodes     1 (+1 tombstoned)"));
    CHECK(any_line_contains(sb, "edges     0"));
}

TEST_CASE("cli: /info without a project degrades politely") {
    std::vector<std::string> sb;
    CHECK_FALSE(run_cli_command("/info", null_deps, sb));
    CHECK(any_line_contains(sb, "no active project"));
}

// ---- /set, /settings, /port ---------------------------------------------

namespace {

struct SettingsFixture {
    zg::app::Settings settings;
    CliDeps           deps;
    int               saves     = 0;
    int               port_seen = -1;
    int               size_w    = -1;
    int               size_h    = -1;

    SettingsFixture() {
        deps.settings      = &settings;
        deps.save_settings = [this] { ++saves; return true; };
        deps.get_port      = [this] { return settings.telemetry_port; };
        deps.set_port      = [this](int p) { port_seen = p; return true; };
        deps.port_listening = [] { return true; };
        deps.set_window_size = [this](int w, int h) { size_w = w; size_h = h; };
    }
};

}  // namespace

TEST_CASE("cli: /settings lists every persisted value") {
    SettingsFixture f;
    std::vector<std::string> sb;
    run_cli_command("/settings", f.deps, sb);
    CHECK(any_line_contains(sb, "grid  on"));
    CHECK(any_line_contains(sb, "crt   on"));
    CHECK(any_line_contains(sb, "dim   on"));
    CHECK(any_line_contains(sb, "port  7777"));
}

TEST_CASE("cli: /set flips a flag and saves") {
    SettingsFixture f;
    std::vector<std::string> sb;
    run_cli_command("/set grid off", f.deps, sb);
    CHECK_FALSE(f.settings.show_grid);
    CHECK(f.saves == 1);
    run_cli_command("/set crt 0", f.deps, sb);
    CHECK_FALSE(f.settings.post_process);
    run_cli_command("/set dim false", f.deps, sb);
    CHECK_FALSE(f.settings.dim_filtered);
    CHECK(f.saves == 3);
}

TEST_CASE("cli: /set rejects unknown keys and bad values") {
    SettingsFixture f;
    std::vector<std::string> sb;
    run_cli_command("/set volume 11", f.deps, sb);
    CHECK(any_line_contains(sb, "unknown setting"));
    run_cli_command("/set grid maybe", f.deps, sb);
    CHECK(f.settings.show_grid);  // untouched
    CHECK(f.saves == 0);
}

TEST_CASE("cli: /set size resizes the window and persists") {
    SettingsFixture f;
    std::vector<std::string> sb;
    run_cli_command("/set size 1600x900", f.deps, sb);
    CHECK(f.size_w == 1600);
    CHECK(f.size_h == 900);
    CHECK(f.settings.window_w == 1600);
    CHECK(f.settings.window_h == 900);
    CHECK(f.saves == 1);
}

TEST_CASE("cli: /set size rejects malformed or out-of-range values") {
    SettingsFixture f;
    std::vector<std::string> sb;
    run_cli_command("/set size banana", f.deps, sb);
    run_cli_command("/set size 1600", f.deps, sb);
    run_cli_command("/set size 100x100", f.deps, sb);
    run_cli_command("/set size 1600x90000", f.deps, sb);
    run_cli_command("/set size 1600x900x2", f.deps, sb);
    CHECK(f.size_w == -1);
    CHECK(f.settings.window_w == 1280);
    CHECK(f.settings.window_h == 800);
    CHECK(f.saves == 0);
    CHECK(any_line_contains(sb, "WxH"));
}

TEST_CASE("cli: /settings lists the window size") {
    SettingsFixture f;
    std::vector<std::string> sb;
    run_cli_command("/settings", f.deps, sb);
    CHECK(any_line_contains(sb, "size  1280x800"));
}

TEST_CASE("cli: /set port rebinds the listener and persists") {
    SettingsFixture f;
    std::vector<std::string> sb;
    run_cli_command("/set port 9000", f.deps, sb);
    CHECK(f.port_seen == 9000);
    CHECK(f.settings.telemetry_port == 9000);
    CHECK(f.saves == 1);
}

TEST_CASE("cli: /port <n> is shorthand for /set port") {
    SettingsFixture f;
    std::vector<std::string> sb;
    run_cli_command("/port 8123", f.deps, sb);
    CHECK(f.port_seen == 8123);
    CHECK(f.settings.telemetry_port == 8123);
}

TEST_CASE("cli: /port rejects out-of-range values without touching anything") {
    SettingsFixture f;
    std::vector<std::string> sb;
    run_cli_command("/port 0", f.deps, sb);
    run_cli_command("/port 99999", f.deps, sb);
    run_cli_command("/port banana", f.deps, sb);
    CHECK(f.port_seen == -1);
    CHECK(f.settings.telemetry_port == 7777);
    CHECK(f.saves == 0);
    CHECK(any_line_contains(sb, "1-65535"));
}

TEST_CASE("cli: /port with no args reports the current port and bind state") {
    SettingsFixture f;
    std::vector<std::string> sb;
    run_cli_command("/port", f.deps, sb);
    CHECK(any_line_contains(sb, "udp 127.0.0.1:7777 (listening)"));
}

// ---- /phantom, /phantoms, /filter ----------------------------------------

namespace {

struct PhantomFixture : Fixture {
    zg::telemetry::PhantomBuffer        buffer;
    std::vector<zg::telemetry::Phantom> snapshot;

    PhantomFixture() {
        session.current_project = "default";
        deps.phantom_buffer     = &buffer;
        deps.phantoms           = &snapshot;
    }
    // Pull everything out of the buffer the way the render loop would.
    std::vector<zg::telemetry::Phantom> drain() {
        std::vector<zg::telemetry::Phantom> out;
        buffer.snapshot_and_expire(out, /*ttl=*/3600.0f, /*now=*/0.0);
        return out;
    }
};

}  // namespace

TEST_CASE("cli: /phantom injects one phantom tagged cli + current project") {
    PhantomFixture f;
    std::vector<std::string> sb;
    run_cli_command("/phantom \"ghost ship\"", f.deps, sb);
    const auto out = f.drain();
    REQUIRE(out.size() == 1);
    CHECK(out[0].label == "ghost ship");
    CHECK(out[0].source == "cli");
    CHECK(out[0].project == "default");
    CHECK(out[0].category.empty());
}

TEST_CASE("cli: /phantom --n spawns a numbered batch with one category") {
    PhantomFixture f;
    std::vector<std::string> sb;
    run_cli_command("/phantom decoy --n 5 --cat recon", f.deps, sb);
    const auto out = f.drain();
    REQUIRE(out.size() == 5);
    for (const auto& p : out) CHECK(p.category == "recon");
    CHECK(any_line_contains(sb, "injected 5 phantoms [recon]"));
    // Distinct ids and numbered labels so a batch is tellable-apart.
    CHECK(out[0].id != out[1].id);
    CHECK(out[0].label == "decoy-1");
    CHECK(out[4].label == "decoy-5");
}

TEST_CASE("cli: /phantom bounds the batch size") {
    PhantomFixture f;
    std::vector<std::string> sb;
    run_cli_command("/phantom x --n 0", f.deps, sb);
    run_cli_command("/phantom x --n 101", f.deps, sb);
    CHECK(f.drain().empty());
    CHECK(any_line_contains(sb, "--n must be 1-100"));
}

TEST_CASE("cli: /phantoms lists the snapshot, optionally by category") {
    PhantomFixture f;
    zg::telemetry::Phantom a{};
    a.id = 1; a.label = "alpha"; a.category = "recon";
    zg::telemetry::Phantom b{};
    b.id = 2; b.label = "bravo"; b.category = "exfil";
    f.snapshot = {a, b};

    std::vector<std::string> sb;
    run_cli_command("/phantoms", f.deps, sb);
    CHECK(any_line_contains(sb, "2 phantoms"));

    sb.clear();
    run_cli_command("/phantoms recon", f.deps, sb);
    CHECK(any_line_contains(sb, "alpha"));
    CHECK_FALSE(any_line_contains(sb, "bravo"));
    CHECK(any_line_contains(sb, "1 phantom in [recon]"));
}

TEST_CASE("cli: /pin promotes a phantom through the shared pin path") {
    PhantomFixture f;
    // promote_phantom bounds connection targets by positions.size(), same
    // as the click path — the fixture needs the physics snapshot mirrored.
    f.session.positions = {Vector3{0, 0, 0}, Vector3{5, 0, 5}};
    std::unordered_map<long long, double> tracker;
    f.deps.spawn_tracker = &tracker;

    zg::telemetry::Phantom p{};
    p.id    = 41;
    p.label = "ghost";
    p.connections.push_back({0, "knows"});
    f.buffer.add(p);
    f.snapshot = {p};
    tracker[41] = 0.0;

    std::vector<std::string> sb;
    run_cli_command("/pin 41", f.deps, sb);
    REQUIRE(f.session.stored_nodes.size() == 3);
    CHECK(f.session.stored_nodes[2].title == "ghost");
    CHECK(f.session.stored_nodes[2].tier == "phantom");   // trust gradient
    REQUIRE(f.session.edges.size() == 1);
    CHECK(f.session.edges[0].certainty == "phantom");
    CHECK(f.session.selected_node == 2);
    CHECK(f.drain().empty());                  // removed from the buffer
    CHECK(tracker.find(41) == tracker.end());  // no decay misfire later
    CHECK(any_line_contains(sb, "pinned"));
}

TEST_CASE("cli: /pin with an unknown id pins nothing") {
    PhantomFixture f;
    std::vector<std::string> sb;
    run_cli_command("/pin 999", f.deps, sb);
    CHECK(f.session.stored_nodes.size() == 2);
    CHECK(any_line_contains(sb, "no active phantom"));
    run_cli_command("/pin banana", f.deps, sb);
    CHECK(any_line_contains(sb, "usage:"));
}

TEST_CASE("cli: /decay removes one phantom; the lifecycle diff logs it") {
    PhantomFixture f;
    zg::telemetry::Phantom a{};
    a.id = 1;
    zg::telemetry::Phantom b{};
    b.id = 2;
    f.buffer.add(a);
    f.buffer.add(b);
    f.snapshot = {a, b};

    std::vector<std::string> sb;
    run_cli_command("/decay 1", f.deps, sb);
    const auto left = f.drain();
    REQUIRE(left.size() == 1);
    CHECK(left[0].id == 2);
    CHECK(any_line_contains(sb, "dismissed"));

    sb.clear();
    run_cli_command("/decay 7", f.deps, sb);
    CHECK(any_line_contains(sb, "no active phantom"));
}

TEST_CASE("cli: /decay all empties the buffer") {
    PhantomFixture f;
    zg::telemetry::Phantom a{};
    a.id = 1;
    f.buffer.add(a);
    f.snapshot = {a};
    std::vector<std::string> sb;
    run_cli_command("/decay all", f.deps, sb);
    CHECK(f.drain().empty());
    CHECK(any_line_contains(sb, "dismissed 1 phantom"));
}

TEST_CASE("cli: /filter sets, reports, and clears the category filter") {
    PhantomFixture f;
    std::vector<std::string> sb;
    run_cli_command("/filter recon", f.deps, sb);
    CHECK(f.session.phantom_filter == "recon");

    sb.clear();
    run_cli_command("/filter", f.deps, sb);
    CHECK(any_line_contains(sb, "phantom filter: recon"));

    run_cli_command("/filter all", f.deps, sb);
    CHECK(f.session.phantom_filter.empty());
}
