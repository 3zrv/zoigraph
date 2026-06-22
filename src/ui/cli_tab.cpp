#include "ui/cli_tab.h"

#include <imgui.h>
#include <imgui_stdlib.h>
#include <raylib.h>
#include <raymath.h>

#include <nlohmann/json.hpp>

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

#include "app/clock.h"
#include "app/pin.h"
#include "graph/types.h"
#include "persistence/db.h"
#include "persistence/project_store.h"

namespace zg::ui {

namespace {

// Single source of truth for Tab completion. The dispatcher at the bottom
// stays an explicit if-chain — at this size a table indirection would buy
// nothing — but any command added there must be added here too (test_cli
// pins a few so a drift shows up as a red completion test).
constexpr const char* kCommands[] = {
    "/ask", "/clear", "/decay", "/delete", "/edge", "/filter", "/help",
    "/info", "/neighbors", "/node", "/panic", "/phantom", "/phantoms",
    "/pin", "/port", "/project", "/projects", "/search", "/select", "/set",
    "/settings", "/tier",
};

}  // namespace

std::optional<std::string> cli_history_step(CliState& cli, int dir,
                                            const std::string& current) {
    if (cli.history.empty()) return std::nullopt;
    const int last = static_cast<int>(cli.history.size()) - 1;
    if (dir < 0) {  // older
        if (cli.hist_pos == 0) return std::nullopt;  // already at the oldest
        if (cli.hist_pos < 0) {
            cli.stash    = current;  // first Up: save the draft
            cli.hist_pos = last;
        } else {
            --cli.hist_pos;
        }
        return cli.history[static_cast<std::size_t>(cli.hist_pos)];
    }
    // newer
    if (cli.hist_pos < 0) return std::nullopt;  // already on the fresh line
    if (cli.hist_pos == last) {
        cli.hist_pos = -1;
        return cli.stash;
    }
    ++cli.hist_pos;
    return cli.history[static_cast<std::size_t>(cli.hist_pos)];
}

void cli_history_push(CliState& cli, const std::string& line) {
    cli.hist_pos = -1;
    cli.stash.clear();
    if (line.find_first_not_of(" \t") == std::string::npos) return;
    if (!cli.history.empty() && cli.history.back() == line) return;
    cli.history.push_back(line);
}

CliCompletion cli_complete(const std::string& input) {
    CliCompletion out{input, {}};
    // Only the command word completes: a leading slash and no whitespace
    // yet (mid-argument Tab is a deliberate no-op, not a file completer).
    if (input.empty() || input[0] != '/') return out;
    if (input.find_first_of(" \t") != std::string::npos) return out;
    for (const char* cmd : kCommands) {
        if (std::string_view(cmd).substr(0, input.size()) == input) {
            out.candidates.push_back(cmd);
        }
    }
    if (out.candidates.empty()) return out;
    if (out.candidates.size() == 1) {
        out.completed = out.candidates[0] + std::string(" ");
        return out;
    }
    // Ambiguous: extend to the longest common prefix of the candidates.
    std::string lcp = out.candidates[0];
    for (const auto& c : out.candidates) {
        std::size_t i = 0;
        while (i < lcp.size() && i < c.size() && lcp[i] == c[i]) ++i;
        lcp.resize(i);
    }
    out.completed = lcp;
    return out;
}

std::vector<std::string> cli_tokenize(const std::string& line) {
    std::vector<std::string> out;
    std::string cur;
    bool in_quotes = false;
    bool has_token = false;  // distinguishes "" (empty quoted token) from nothing
    for (char c : line) {
        if (c == '"') {
            in_quotes = !in_quotes;
            has_token = true;
            continue;
        }
        if (!in_quotes && (c == ' ' || c == '\t')) {
            if (has_token) out.push_back(cur);
            cur.clear();
            has_token = false;
            continue;
        }
        cur += c;
        has_token = true;
    }
    if (has_token) out.push_back(cur);
    return out;
}

namespace {

using Scrollback = std::vector<std::string>;

void cmd_help(Scrollback& sb) {
    sb.push_back("/node \"title\" [--tier t] [--to ref]   create node (+edge to ref)");
    sb.push_back("/edge <a> <b> [kind]                  edge between two nodes");
    sb.push_back("        node refs are an id, an exact title, or a search term");
    sb.push_back("/search <terms...>                    FTS search; jumps to top hit");
    sb.push_back("/select <ref>                         select + fly to a node");
    sb.push_back("/neighbors <ref>                      list a node's edges");
    sb.push_back("/tier <ref> <t>                       set a node's tier");
    sb.push_back("/delete <ref> [--confirm]             tombstone a node");
    sb.push_back("/projects                             list projects (* = active)");
    sb.push_back("/project <name> [--create]            switch (or create) project");
    sb.push_back("/info                                 active-project stats");
    sb.push_back("/phantom \"label\" [--n N] [--cat c]    inject local phantom(s)");
    sb.push_back("/phantoms [cat]                       list active phantoms");
    sb.push_back("/pin <phantom-id>                     accept: promote to a node");
    sb.push_back("/decay <phantom-id|all>               dismiss without pinning");
    sb.push_back("/ask [ref]                            LLM phantoms about a node");
    sb.push_back("/filter <cat|all>                     show only one phantom category");
    sb.push_back("/settings                             show persisted settings");
    sb.push_back("/set <grid|crt|dim> <on|off>          flip + persist a view flag");
    sb.push_back("/set port <n>  (or /port <n>)         rebind the UDP listener");
    sb.push_back("/set qport <n>                        rebind the query channel");
    sb.push_back("/set size <WxH>                       resize + persist the window");
    sb.push_back("/clear                                clear this scrollback");
    sb.push_back("/panic   overwrite + delete ALL project data, then exit");
    sb.push_back("/help    this list");
    sb.push_back("up/down recall history; tab completes a command");
}

// Resolves a node reference to an index into stored_nodes, or -1. Three
// rungs, cheapest first: numeric id, exact title match, FTS top hit.
// Tombstoned nodes never resolve — every command that takes a ref means
// "a node the operator can see".
long long resolve_node(zg::app::Session& s, const std::string& ref) {
    const auto& nodes = s.stored_nodes;
    char* end = nullptr;
    const long long as_id = std::strtoll(ref.c_str(), &end, 10);
    if (end && *end == '\0' && !ref.empty()) {
        if (as_id >= 0 && static_cast<std::size_t>(as_id) < nodes.size()
            && !nodes[static_cast<std::size_t>(as_id)].deleted) {
            return as_id;
        }
        return -1;  // a numeric ref that misses shouldn't fall through to FTS
    }
    for (const auto& n : nodes) {
        if (!n.deleted && n.title == ref) return n.id;
    }
    if (s.db) {
        for (long long hit : s.db->search(ref)) {
            if (hit >= 0 && static_cast<std::size_t>(hit) < nodes.size()
                && !nodes[static_cast<std::size_t>(hit)].deleted) {
                return hit;
            }
        }
    }
    return -1;
}

const std::string& node_title(const zg::app::Session& s, long long id) {
    static const std::string untitled = "(untitled)";
    const auto& t = s.stored_nodes[static_cast<std::size_t>(id)].title;
    return t.empty() ? untitled : t;
}

// Select `idx` and fly the camera to it preserving the current orbit
// offset — the same move the inspector's live search makes. Selection
// applies even when the position isn't known yet (no physics snapshot);
// the camera just stays put in that case.
void select_and_fly(CliDeps& d, zg::app::Session& s, std::size_t idx) {
    s.selected_node = static_cast<int>(idx);
    if (d.camera && idx < s.positions.size()) {
        const Vector3 offset =
            Vector3Subtract(d.camera->position, d.camera->target);
        d.camera->target   = s.positions[idx];
        d.camera->position = Vector3Add(d.camera->target, offset);
    }
}

void cmd_select(const std::vector<std::string>& args,
                CliDeps& d, Scrollback& sb) {
    if (!d.session) {
        sb.push_back("no session");
        return;
    }
    if (args.size() != 2) {
        sb.push_back("usage: /select <ref>");
        return;
    }
    auto& s = *d.session;
    const long long id = resolve_node(s, args[1]);
    if (id < 0) {
        sb.push_back("no node matching '" + args[1] + "'");
        return;
    }
    select_and_fly(d, s, static_cast<std::size_t>(id));
    sb.push_back("selected " + std::to_string(id) + " '" + node_title(s, id) +
                 "'");
}

void cmd_neighbors(const std::vector<std::string>& args,
                   CliDeps& d, Scrollback& sb) {
    if (!d.session) {
        sb.push_back("no session");
        return;
    }
    if (args.size() != 2) {
        sb.push_back("usage: /neighbors <ref>");
        return;
    }
    auto& s = *d.session;
    const long long id = resolve_node(s, args[1]);
    if (id < 0) {
        sb.push_back("no node matching '" + args[1] + "'");
        return;
    }
    const auto self = static_cast<std::size_t>(id);
    std::size_t shown = 0;
    for (const auto& e : s.edges) {
        if (e.source != self && e.target != self) continue;
        const std::size_t other = e.source == self ? e.target : e.source;
        // Edges to tombstoned counterparts are invisible in the field;
        // keep the listing consistent with what the operator can see.
        if (other >= s.stored_nodes.size() || s.stored_nodes[other].deleted) {
            continue;
        }
        std::string line = "  " + std::to_string(other) + "  " +
                           node_title(s, static_cast<long long>(other));
        if (!e.kind.empty()) line += " (" + e.kind + ")";
        if (!e.certainty.empty()) line += " [" + e.certainty + "]";
        sb.push_back(line);
        ++shown;
    }
    if (shown == 0) {
        sb.push_back("no edges on " + node_title(s, id));
    } else {
        sb.push_back(std::to_string(shown) + " edge" + (shown == 1 ? "" : "s"));
    }
}

// Append node + optional edge through the same hot path the toolbar uses:
// stored_nodes push + enqueue + incremental DB insert. Physics may be null
// under test; the enqueue is skipped and positions catch up via load.
void cmd_node(const std::vector<std::string>& args,
              CliDeps& d, Scrollback& sb) {
    if (!d.session || !d.session->db) {
        sb.push_back("no active project");
        return;
    }
    if (args.size() < 2 || args[1].empty()) {
        sb.push_back("usage: /node \"title\" [--tier confirmed|suspected|phantom] [--to ref]");
        return;
    }
    auto& s = *d.session;
    const std::string& title = args[1];
    std::string tier = "confirmed";
    std::string to_ref;
    for (std::size_t i = 2; i + 1 < args.size(); i += 2) {
        if (args[i] == "--tier") {
            tier = args[i + 1];
        } else if (args[i] == "--to") {
            to_ref = args[i + 1];
        } else {
            sb.push_back("unknown option: " + args[i]);
            return;
        }
    }
    if (tier != "confirmed" && tier != "suspected" && tier != "phantom") {
        sb.push_back("tier must be confirmed, suspected, or phantom");
        return;
    }
    long long target = -1;
    if (!to_ref.empty()) {
        target = resolve_node(s, to_ref);
        if (target < 0) {
            sb.push_back("no node matching '" + to_ref + "'");
            return;
        }
    }

    const double    now_ts = zg::app::unix_now();
    const long long new_id = static_cast<long long>(s.stored_nodes.size());
    const float     angle  = 0.7f * static_cast<float>(new_id);
    Vector3 spawn{8.0f * std::cos(angle), 0.0f, 8.0f * std::sin(angle)};
    if (target >= 0) {
        // Spawn beside the edge target so the new spring doesn't yank it
        // across the field on the first tick (same pattern as the toolbar).
        const auto t = static_cast<std::size_t>(target);
        const Vector3 a = t < s.positions.size() ? s.positions[t]
                                                 : s.stored_nodes[t].position;
        spawn = Vector3{a.x + 3.0f * std::cos(angle), a.y + 1.0f,
                        a.z + 3.0f * std::sin(angle)};
    }
    zg::persistence::StoredNode n{};
    n.id           = new_id;
    n.position     = spawn;
    n.title        = title;
    n.first_seen   = now_ts;
    n.last_touched = now_ts;
    n.tier         = tier;
    s.stored_nodes.push_back(std::move(n));
    if (s.physics) s.physics->enqueue_node(spawn);
    s.db->insert_node(s.stored_nodes.back());

    if (target >= 0) {
        const zg::graph::Edge e{static_cast<std::size_t>(new_id),
                                static_cast<std::size_t>(target),
                                "", "", "confirmed"};
        s.edges.push_back(e);
        if (s.physics) s.physics->enqueue_edge(e);
        s.db->insert_edge(e);
        sb.push_back("node " + std::to_string(new_id) + " '" + title +
                     "' -> " + node_title(s, target));
    } else {
        sb.push_back("node " + std::to_string(new_id) + " '" + title + "'");
    }
}

void cmd_edge(const std::vector<std::string>& args,
              CliDeps& d, Scrollback& sb) {
    if (!d.session || !d.session->db) {
        sb.push_back("no active project");
        return;
    }
    if (args.size() < 3 || args.size() > 4) {
        sb.push_back("usage: /edge <a> <b> [kind]");
        return;
    }
    auto& s = *d.session;
    const long long a = resolve_node(s, args[1]);
    if (a < 0) { sb.push_back("no node matching '" + args[1] + "'"); return; }
    const long long b = resolve_node(s, args[2]);
    if (b < 0) { sb.push_back("no node matching '" + args[2] + "'"); return; }
    if (a == b) { sb.push_back("can't edge a node to itself"); return; }
    const std::string kind = args.size() == 4 ? args[3] : "";

    const zg::graph::Edge e{static_cast<std::size_t>(a),
                            static_cast<std::size_t>(b),
                            "", kind, "confirmed"};
    s.edges.push_back(e);
    if (s.physics) s.physics->enqueue_edge(e);
    s.db->insert_edge(e);
    sb.push_back("edge " + node_title(s, a) + " -> " + node_title(s, b) +
                 (kind.empty() ? "" : " (" + kind + ")"));
}

void cmd_ask(const std::vector<std::string>& args,
             CliDeps& d, Scrollback& sb) {
    if (!d.session || !d.session->db) {
        sb.push_back("no active project");
        return;
    }
    if (args.size() > 2) {
        sb.push_back("usage: /ask [ref]   (defaults to the selection)");
        return;
    }
    auto& s = *d.session;
    long long id = -1;
    if (args.size() == 2) {
        id = resolve_node(s, args[1]);
        if (id < 0) {
            sb.push_back("no node matching '" + args[1] + "'");
            return;
        }
    } else {
        if (s.selected_node < 0
            || static_cast<std::size_t>(s.selected_node) >= s.stored_nodes.size()
            || s.stored_nodes[static_cast<std::size_t>(s.selected_node)].deleted) {
            sb.push_back("nothing selected; /ask <ref> or /select one first");
            return;
        }
        id = s.selected_node;
    }
    // Same single-flight rule as the inspector button: never double-fire
    // the subprocess.
    const auto snap = s.ask.snapshot();
    if (snap.state == zg::app::LlmAsk::State::Thinking) {
        sb.push_back("an ask is already in flight" +
                     (snap.msg.empty() ? "" : ": " + snap.msg));
        return;
    }
    if (!d.ask_start) {
        sb.push_back("ask unavailable");
        return;
    }
    d.ask_start(id);
    sb.push_back("asking about node " + std::to_string(id) + " '" +
                 node_title(s, id) +
                 "' -- phantoms land via UDP (/phantoms to list)");
}

void cmd_tier(const std::vector<std::string>& args,
              CliDeps& d, Scrollback& sb) {
    if (!d.session || !d.session->db) {
        sb.push_back("no active project");
        return;
    }
    if (args.size() != 3) {
        sb.push_back("usage: /tier <ref> <confirmed|suspected|phantom>");
        return;
    }
    auto& s = *d.session;
    const std::string& tier = args[2];
    if (tier != "confirmed" && tier != "suspected" && tier != "phantom") {
        sb.push_back("tier must be confirmed, suspected, or phantom");
        return;
    }
    const long long id = resolve_node(s, args[1]);
    if (id < 0) {
        sb.push_back("no node matching '" + args[1] + "'");
        return;
    }
    if (static_cast<std::size_t>(id) == s.self_idx) {
        sb.push_back("the self node is structural; its tier can't change");
        return;
    }
    s.stored_nodes[static_cast<std::size_t>(id)].tier = tier;
    s.db->update_node_tier(id, tier);
    sb.push_back(node_title(s, id) + " -> " + tier);
}

void cmd_delete(const std::vector<std::string>& args,
                CliDeps& d, Scrollback& sb) {
    if (!d.session || !d.session->db) {
        sb.push_back("no active project");
        return;
    }
    const bool confirm = args.size() == 3 && args[2] == "--confirm";
    if (args.size() < 2 || (args.size() == 3 && !confirm) || args.size() > 3) {
        sb.push_back("usage: /delete <ref> [--confirm]");
        return;
    }
    auto& s = *d.session;
    const long long id = resolve_node(s, args[1]);
    if (id < 0) {
        sb.push_back("no node matching '" + args[1] + "'");
        return;
    }
    if (static_cast<std::size_t>(id) == s.self_idx) {
        sb.push_back("the self node is structural; it can't be deleted");
        return;
    }
    auto& sn = s.stored_nodes[static_cast<std::size_t>(id)];
    if (!confirm) {
        // Stateless arm/confirm: the instruction embeds the RESOLVED id,
        // so the confirming command targets exactly what resolved here
        // even if a fuzzy ref would resolve differently by then. No armed
        // state to go stale — T0.7's bug class can't exist.
        sb.push_back("node " + std::to_string(id) + " '" + node_title(s, id) +
                     "' -- to delete, type: /delete " + std::to_string(id) +
                     " --confirm");
        return;
    }
    // Same side-effects as the inspector's delete button: tombstone,
    // persist, log a node_delete event (phase-2 pin-then-delete metric),
    // drop the selection if it pointed here. Row stays in the DB so the
    // phantom_pin -> node_delete events join still finds the pin.
    sn.deleted = true;
    s.db->mark_deleted(sn.id);
    if (s.physics) s.physics->set_node_disabled(static_cast<std::size_t>(sn.id));
    {
        nlohmann::json p = {
            {"node_id",     sn.id},
            {"title_len",   sn.title.size()},
            {"content_len", sn.content.size()},
            {"tier",        sn.tier},
        };
        s.db->log_event("node_delete", sn.id, p.dump());
    }
    if (s.selected_node == static_cast<int>(id)) s.selected_node = -1;
    sb.push_back("deleted node " + std::to_string(id) + " '" +
                 node_title(s, id) + "'");
}

void cmd_phantom(const std::vector<std::string>& args,
                 CliDeps& d, Scrollback& sb) {
    if (!d.phantom_buffer) {
        sb.push_back("phantom buffer unavailable");
        return;
    }
    if (args.size() < 2 || args[1].empty()) {
        sb.push_back("usage: /phantom \"label\" [--n N] [--cat category]");
        return;
    }
    const std::string& label = args[1];
    long long count = 1;
    std::string category;
    for (std::size_t i = 2; i + 1 < args.size(); i += 2) {
        if (args[i] == "--n") {
            count = std::strtoll(args[i + 1].c_str(), nullptr, 10);
        } else if (args[i] == "--cat") {
            category = args[i + 1];
        } else {
            sb.push_back("unknown option: " + args[i]);
            return;
        }
    }
    if (count < 1 || count > 100) {
        sb.push_back("--n must be 1-100");
        return;
    }

    // Same id range as nothing else: the toolbar injector counts from 1e6,
    // UDP senders pick their own (usually small) ids; CLI phantoms start
    // at 2e6 so the three sources can't collide.
    static long long cli_phantom_id = 2000000;
    const double now = zg::app::mono_now();  // same base as TTL expiry / pin timing
    for (long long i = 0; i < count; ++i) {
        zg::telemetry::Phantom p{};
        p.id         = cli_phantom_id++;
        // Ring above the field, spread by index so a batch doesn't land
        // as one overlapping clump.
        const float ang = 0.9f * static_cast<float>(i);
        p.position   = {6.0f * std::cos(ang),
                        6.0f + 0.4f * static_cast<float>(i % 7),
                        6.0f * std::sin(ang)};
        p.label      = count == 1 ? label
                                  : label + "-" + std::to_string(i + 1);
        p.category   = category;
        p.source     = "cli";
        p.spawn_time = now;
        if (d.session) p.project = d.session->current_project;
        d.phantom_buffer->add(std::move(p));
    }
    sb.push_back("injected " + std::to_string(count) + " phantom" +
                 (count == 1 ? "" : "s") +
                 (category.empty() ? "" : " [" + category + "]"));
}

void cmd_pin(const std::vector<std::string>& args,
             CliDeps& d, Scrollback& sb) {
    if (!d.session || !d.session->db) {
        sb.push_back("no active project");
        return;
    }
    if (!d.phantoms || !d.phantom_buffer) {
        sb.push_back("phantom snapshot unavailable");
        return;
    }
    char* end = nullptr;
    const long long id = args.size() == 2
        ? std::strtoll(args[1].c_str(), &end, 10) : -1;
    if (args.size() != 2 || !end || *end != '\0' || args[1].empty()) {
        sb.push_back("usage: /pin <phantom-id>   (/phantoms lists ids)");
        return;
    }
    const zg::telemetry::Phantom* hit = nullptr;
    for (const auto& ph : *d.phantoms) {
        if (ph.id == id) { hit = &ph; break; }
    }
    if (!hit) {
        sb.push_back("no active phantom with id " + std::to_string(id) +
                     " (/phantoms lists them)");
        return;
    }
    // Same path as click-to-pin (app/pin.cpp): identical side-effects and
    // an identical phantom_pin event, so phase-2 metrics can't tell the
    // two accept gestures apart except via time_to_pin_s.
    std::unordered_map<long long, double> no_tracker;
    const long long new_id = zg::app::pin_phantom(
        *d.session, *hit, *d.phantom_buffer,
        d.spawn_tracker ? *d.spawn_tracker : no_tracker);
    sb.push_back("pinned phantom " + std::to_string(id) + " -> node " +
                 std::to_string(new_id) + " '" +
                 node_title(*d.session, new_id) + "'");
}

void cmd_decay(const std::vector<std::string>& args,
               CliDeps& d, Scrollback& sb) {
    if (!d.phantoms || !d.phantom_buffer) {
        sb.push_back("phantom snapshot unavailable");
        return;
    }
    if (args.size() != 2) {
        sb.push_back("usage: /decay <phantom-id|all>");
        return;
    }
    // Dismissal is just early removal from the buffer: the next frame's
    // lifecycle diff sees the phantom gone and logs an ordinary
    // phantom_decay (lifetime = time-of-dismissal). No new event kind,
    // and a dismissal counts as a non-pin in the phase-2 metrics, which
    // is exactly what it is.
    if (args[1] == "all") {
        const std::size_t n = d.phantoms->size();
        d.phantom_buffer->clear();
        sb.push_back("dismissed " + std::to_string(n) + " phantom" +
                     (n == 1 ? "" : "s"));
        return;
    }
    char* end = nullptr;
    const long long id = std::strtoll(args[1].c_str(), &end, 10);
    if (!end || *end != '\0' || args[1].empty()) {
        sb.push_back("usage: /decay <phantom-id|all>");
        return;
    }
    bool found = false;
    for (const auto& ph : *d.phantoms) {
        if (ph.id == id) { found = true; break; }
    }
    if (!found) {
        sb.push_back("no active phantom with id " + std::to_string(id));
        return;
    }
    d.phantom_buffer->remove(id);
    sb.push_back("dismissed phantom " + std::to_string(id));
}

void cmd_phantoms(const std::vector<std::string>& args,
                  CliDeps& d, Scrollback& sb) {
    if (!d.phantoms) {
        sb.push_back("phantom snapshot unavailable");
        return;
    }
    const std::string cat = args.size() > 1 ? args[1] : "";
    std::size_t shown = 0;
    for (const auto& ph : *d.phantoms) {
        if (!cat.empty() && ph.category != cat) continue;
        std::string line = "  " + std::to_string(ph.id) + "  " +
                           (ph.label.empty() ? "(unlabeled)" : ph.label);
        if (!ph.category.empty()) line += " [" + ph.category + "]";
        if (!ph.source.empty())   line += " <" + ph.source + ">";
        sb.push_back(line);
        ++shown;
    }
    sb.push_back(std::to_string(shown) + " phantom" + (shown == 1 ? "" : "s") +
                 (cat.empty() ? "" : " in [" + cat + "]"));
}

void cmd_filter(const std::vector<std::string>& args,
                CliDeps& d, Scrollback& sb) {
    if (!d.session) {
        sb.push_back("no session");
        return;
    }
    auto& filter = d.session->phantom_filter;
    if (args.size() == 1) {
        sb.push_back(filter.empty() ? "phantom filter: (all)"
                                    : "phantom filter: " + filter);
        return;
    }
    if (args.size() != 2) {
        sb.push_back("usage: /filter <category|all>");
        return;
    }
    if (args[1] == "all") {
        filter.clear();
        sb.push_back("phantom filter cleared");
    } else {
        filter = args[1];
        sb.push_back("showing only phantoms in [" + filter + "]");
    }
}

int parse_bool(const std::string& v) {
    if (v == "on" || v == "true" || v == "1") return 1;
    if (v == "off" || v == "false" || v == "0") return 0;
    return -1;
}

void cmd_settings(CliDeps& d, Scrollback& sb) {
    if (!d.settings) { sb.push_back("settings unavailable"); return; }
    const auto& st = *d.settings;
    sb.push_back(std::string("grid  ") + (st.show_grid    ? "on" : "off"));
    sb.push_back(std::string("crt   ") + (st.post_process ? "on" : "off"));
    sb.push_back(std::string("dim   ") + (st.dim_filtered ? "on" : "off"));
    sb.push_back("port  " + std::to_string(st.telemetry_port));
    sb.push_back("qport " + std::to_string(st.query_port));
    sb.push_back("size  " + std::to_string(st.window_w) + "x" +
                 std::to_string(st.window_h));
}

void apply_window_size(const std::string& val, CliDeps& d, Scrollback& sb) {
    if (!d.settings) { sb.push_back("settings unavailable"); return; }
    // Strict WxH parse: two integers, one 'x', nothing else.
    const auto xpos = val.find('x');
    char* end = nullptr;
    const long w = xpos != std::string::npos && xpos > 0
        ? std::strtol(val.substr(0, xpos).c_str(), &end, 10) : 0;
    const bool w_ok = end && *end == '\0';
    end = nullptr;
    const long h = xpos != std::string::npos
        ? std::strtol(val.substr(xpos + 1).c_str(), &end, 10) : 0;
    const bool h_ok = end && *end == '\0' && xpos + 1 < val.size();
    if (!w_ok || !h_ok
        || w < zg::app::kMinWindowW || w > zg::app::kMaxWindowW
        || h < zg::app::kMinWindowH || h > zg::app::kMaxWindowH) {
        sb.push_back("size must be WxH, " +
                     std::to_string(zg::app::kMinWindowW) + "x" +
                     std::to_string(zg::app::kMinWindowH) + " to " +
                     std::to_string(zg::app::kMaxWindowW) + "x" +
                     std::to_string(zg::app::kMaxWindowH));
        return;
    }
    d.settings->window_w = static_cast<int>(w);
    d.settings->window_h = static_cast<int>(h);
    if (d.set_window_size) {
        d.set_window_size(static_cast<int>(w), static_cast<int>(h));
    }
    if (d.save_settings && !d.save_settings()) {
        sb.push_back("warning: settings file not writable");
    }
    sb.push_back("window -> " + std::to_string(w) + "x" + std::to_string(h) +
                 " (saved)");
}

void apply_port(int port, CliDeps& d, Scrollback& sb) {
    if (port < 1 || port > 65535) {
        sb.push_back("port must be 1-65535");
        return;
    }
    if (!d.settings) { sb.push_back("settings unavailable"); return; }
    if (d.set_port) {
        d.set_port(port);  // restarts the listener; bind lands async
    }
    d.settings->telemetry_port = port;
    if (d.save_settings && !d.save_settings()) {
        sb.push_back("warning: settings file not writable");
    }
    sb.push_back("udp listener moving to 127.0.0.1:" + std::to_string(port) +
                 " (bind is async; /info shows status)");
}

void apply_query_port(int port, CliDeps& d, Scrollback& sb) {
    if (port < 1 || port > 65535) {
        sb.push_back("qport must be 1-65535");
        return;
    }
    if (!d.settings) { sb.push_back("settings unavailable"); return; }
    if (d.set_query_port) {
        d.set_query_port(port);  // tears down + rebinds the query socket
    }
    d.settings->query_port = port;
    if (d.save_settings && !d.save_settings()) {
        sb.push_back("warning: settings file not writable");
    }
    sb.push_back("query channel moving to 127.0.0.1:" + std::to_string(port) +
                 " (bind is async; /info shows status)");
}

void cmd_set(const std::vector<std::string>& args,
             CliDeps& d, Scrollback& sb) {
    if (args.size() != 3) {
        sb.push_back("usage: /set <grid|crt|dim|port|qport|size> <value>");
        return;
    }
    const std::string& key = args[1];
    const std::string& val = args[2];
    if (key == "port") {
        apply_port(static_cast<int>(std::strtol(val.c_str(), nullptr, 10)),
                   d, sb);
        return;
    }
    if (key == "qport") {
        apply_query_port(static_cast<int>(std::strtol(val.c_str(), nullptr, 10)),
                         d, sb);
        return;
    }
    if (key == "size") {
        apply_window_size(val, d, sb);
        return;
    }
    if (!d.settings) { sb.push_back("settings unavailable"); return; }
    bool* flag = nullptr;
    if (key == "grid") flag = &d.settings->show_grid;
    else if (key == "crt") flag = &d.settings->post_process;
    else if (key == "dim") flag = &d.settings->dim_filtered;
    if (!flag) {
        sb.push_back("unknown setting: " + key +
                     " (grid, crt, dim, port, qport, size)");
        return;
    }
    const int b = parse_bool(val);
    if (b < 0) {
        sb.push_back("value must be on/off");
        return;
    }
    *flag = (b == 1);
    if (d.save_settings && !d.save_settings()) {
        sb.push_back("warning: settings file not writable");
    }
    sb.push_back(key + " = " + (b ? "on" : "off") + " (saved)");
}

void cmd_port(const std::vector<std::string>& args,
              CliDeps& d, Scrollback& sb) {
    if (args.size() == 1) {
        if (!d.get_port) { sb.push_back("port unavailable"); return; }
        std::string status;
        if (d.port_listening) {
            status = d.port_listening() ? " (listening)" : " (NOT bound)";
        }
        sb.push_back("udp 127.0.0.1:" + std::to_string(d.get_port()) + status);
        return;
    }
    if (args.size() != 2) {
        sb.push_back("usage: /port [n]");
        return;
    }
    apply_port(static_cast<int>(std::strtol(args[1].c_str(), nullptr, 10)),
               d, sb);
}

void cmd_search(const std::vector<std::string>& args,
                CliDeps& d, Scrollback& sb) {
    if (!d.session || !d.session->db) {
        sb.push_back("no active project");
        return;
    }
    if (args.size() < 2) {
        sb.push_back("usage: /search <terms...>");
        return;
    }
    auto& s = *d.session;
    std::string query = args[1];
    for (std::size_t i = 2; i < args.size(); ++i) query += ' ' + args[i];

    // Mirror the inspector's state so the two search UIs never disagree:
    // the match count it renders and the hits it steps through are the
    // ones this command just produced.
    s.search_query = query;
    s.search_hits  = s.db->search(query);
    if (s.search_hits.empty()) {
        sb.push_back("no matches for '" + query + "'");
        return;
    }

    constexpr std::size_t kMaxListed = 8;
    sb.push_back(std::to_string(s.search_hits.size()) + " match" +
                 (s.search_hits.size() == 1 ? "" : "es") + ":");
    for (std::size_t i = 0; i < s.search_hits.size() && i < kMaxListed; ++i) {
        const long long id = s.search_hits[i];
        if (id < 0 || static_cast<std::size_t>(id) >= s.stored_nodes.size()) continue;
        sb.push_back("  " + std::to_string(id) + "  " + node_title(s, id));
    }
    if (s.search_hits.size() > kMaxListed) {
        sb.push_back("  ... +" +
                     std::to_string(s.search_hits.size() - kMaxListed) + " more");
    }

    // Select + fly to the top hit, same as the inspector's live search.
    select_and_fly(d, s, static_cast<std::size_t>(s.search_hits.front()));
}

void cmd_projects(CliDeps& d, Scrollback& sb) {
    const auto names = zg::persistence::list_projects(d.projects_dir);
    if (names.empty()) {
        sb.push_back("no projects");
        return;
    }
    const std::string active =
        d.session ? d.session->current_project : std::string();
    for (const auto& n : names) {
        sb.push_back(std::string(n == active ? "* " : "  ") + n);
    }
}

void cmd_project(const std::vector<std::string>& args,
                 CliDeps& d, Scrollback& sb) {
    if (args.size() < 2 || args.size() > 3
        || (args.size() == 3 && args[2] != "--create")) {
        sb.push_back("usage: /project <name> [--create]");
        return;
    }
    if (!d.open_project) {
        sb.push_back("project switching unavailable");
        return;
    }
    const std::string& name   = args[1];
    const bool         create = args.size() == 3;
    if (!zg::persistence::is_valid_project_name(name)) {
        sb.push_back("name must be 1-64 chars: [A-Za-z0-9_-]");
        return;
    }
    if (d.session && name == d.session->current_project) {
        sb.push_back("already on " + name);
        return;
    }
    const bool exists = std::filesystem::exists(
        zg::persistence::project_path(d.projects_dir, name));
    // Switching only opens what already exists; creation is an explicit
    // flag so a typo can't silently seed a fresh 300-node project.
    if (!exists && !create) {
        sb.push_back("no such project: " + name +
                     " (add --create to make it)");
        return;
    }
    if (exists && create) {
        sb.push_back(name + " already exists; switching");
    }
    d.open_project(name);
    sb.push_back("switched to " + name);
}

void cmd_info(CliDeps& d, Scrollback& sb) {
    if (!d.session || !d.session->db) {
        sb.push_back("no active project");
        return;
    }
    auto& s = *d.session;
    sb.push_back("project   " + s.current_project);

    const auto db_path =
        zg::persistence::project_path(d.projects_dir, s.current_project);
    std::error_code ec;
    const auto size = std::filesystem::file_size(db_path, ec);
    sb.push_back("db        " + db_path.string() +
                 (ec ? "" : " (" + std::to_string(size / 1024) + " KiB)"));

    std::size_t live = 0, tombstoned = 0;
    for (const auto& n : s.stored_nodes) (n.deleted ? tombstoned : live)++;
    sb.push_back("nodes     " + std::to_string(live) +
                 " (+" + std::to_string(tombstoned) + " tombstoned)");
    sb.push_back("edges     " + std::to_string(s.edges.size()));
    if (d.phantoms) {
        sb.push_back("phantoms  " + std::to_string(d.phantoms->size()) +
                     " active");
    }
    if (d.get_port) {
        std::string status;
        if (d.port_listening) {
            status = d.port_listening() ? " (listening)" : " (NOT bound)";
        }
        sb.push_back("udp       127.0.0.1:" + std::to_string(d.get_port()) +
                     status);
    }
    if (d.get_query_port) {
        std::string status;
        if (d.query_port_listening) {
            status = d.query_port_listening() ? " (listening)" : " (NOT bound)";
        }
        sb.push_back("query     127.0.0.1:" + std::to_string(d.get_query_port()) +
                     status);
    }
}

}  // namespace

bool run_cli_command(const std::string& line,
                     CliDeps& deps,
                     std::vector<std::string>& scrollback) {
    const std::vector<std::string> args = cli_tokenize(line);
    if (args.empty()) return false;
    const std::string& cmd = args[0];

    // Echo the trimmed-and-tokenized form so what the operator sees in
    // scrollback is what the dispatcher actually parsed.
    {
        std::string echo = ">";
        for (const auto& a : args) {
            echo += ' ';
            echo += a.find(' ') == std::string::npos ? a : '"' + a + '"';
        }
        scrollback.push_back(echo);
    }

    if (cmd == "/panic") {
        // Strict arity: "/panic now" or any other trailing junk does NOT
        // fire — a destructive command only runs when typed exactly.
        if (args.size() > 1) {
            scrollback.push_back("/panic takes no arguments");
            return false;
        }
        scrollback.push_back("wiping all project data and exiting.");
        return true;
    }
    if (cmd == "/help") {
        cmd_help(scrollback);
        return false;
    }
    if (cmd == "/clear") {
        scrollback.clear();  // echo included — the point is an empty pane
        return false;
    }
    if (cmd == "/node") { cmd_node(args, deps, scrollback); return false; }
    if (cmd == "/edge") { cmd_edge(args, deps, scrollback); return false; }
    if (cmd == "/search") { cmd_search(args, deps, scrollback); return false; }
    if (cmd == "/select") { cmd_select(args, deps, scrollback); return false; }
    if (cmd == "/neighbors") { cmd_neighbors(args, deps, scrollback); return false; }
    if (cmd == "/tier") { cmd_tier(args, deps, scrollback); return false; }
    if (cmd == "/delete") { cmd_delete(args, deps, scrollback); return false; }
    if (cmd == "/projects") { cmd_projects(deps, scrollback); return false; }
    if (cmd == "/project") { cmd_project(args, deps, scrollback); return false; }
    if (cmd == "/info") { cmd_info(deps, scrollback); return false; }
    if (cmd == "/phantom") { cmd_phantom(args, deps, scrollback); return false; }
    if (cmd == "/phantoms") { cmd_phantoms(args, deps, scrollback); return false; }
    if (cmd == "/pin") { cmd_pin(args, deps, scrollback); return false; }
    if (cmd == "/decay") { cmd_decay(args, deps, scrollback); return false; }
    if (cmd == "/ask") { cmd_ask(args, deps, scrollback); return false; }
    if (cmd == "/filter") { cmd_filter(args, deps, scrollback); return false; }
    if (cmd == "/settings") { cmd_settings(deps, scrollback); return false; }
    if (cmd == "/set") { cmd_set(args, deps, scrollback); return false; }
    if (cmd == "/port") { cmd_port(args, deps, scrollback); return false; }
    scrollback.push_back("unknown command: " + cmd + " (try /help)");
    return false;
}

bool render_cli_tab(CliState& cli, CliDeps& deps) {
    // Scrollback fills the tab except for one prompt line at the bottom.
    const float prompt_h = ImGui::GetFrameHeightWithSpacing();
    // Horizontal scrollbar: help/scrollback lines are wider than the panel
    // at its default width; without it they'd be silently clipped.
    ImGui::BeginChild("cli_scrollback", ImVec2(0, -prompt_h),
                      ImGuiChildFlags_None,
                      ImGuiWindowFlags_HorizontalScrollbar);
    if (cli.scrollback.empty()) {
        ImGui::TextDisabled("zoigraph cli -- /help for commands");
    }
    for (const auto& l : cli.scrollback) ImGui::TextUnformatted(l.c_str());
    // Stick to the bottom while new lines arrive (the operator can still
    // scroll up; this only kicks in when already at the bottom).
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) {
        ImGui::SetScrollHereY(1.0f);
    }
    ImGui::EndChild();

    ImGui::TextUnformatted(">");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-FLT_MIN);
    bool fired = false;
    // History (Up/Down) and completion (Tab) run inside ImGui's InputText
    // callback because that's the only place the widget lets external code
    // rewrite the buffer mid-edit. The callback shim translates the event
    // into the pure helpers above and copies their result back via
    // DeleteChars/InsertChars (imgui_stdlib keeps cli.input's storage in
    // sync underneath).
    const auto callback = [](ImGuiInputTextCallbackData* data) -> int {
        auto& st = *static_cast<CliState*>(data->UserData);
        const std::string current(data->Buf,
                                  static_cast<std::size_t>(data->BufTextLen));
        std::optional<std::string> replace;
        if (data->EventFlag == ImGuiInputTextFlags_CallbackHistory) {
            replace = cli_history_step(
                st, data->EventKey == ImGuiKey_UpArrow ? -1 : +1, current);
        } else if (data->EventFlag == ImGuiInputTextFlags_CallbackCompletion) {
            auto comp = cli_complete(current);
            if (comp.candidates.size() > 1) {
                std::string line;
                for (const auto& c : comp.candidates) {
                    if (!line.empty()) line += "  ";
                    line += c;
                }
                st.scrollback.push_back(line);
            }
            if (comp.completed != current) replace = std::move(comp.completed);
        }
        if (replace) {
            data->DeleteChars(0, data->BufTextLen);
            data->InsertChars(0, replace->c_str());
        }
        return 0;
    };
    if (ImGui::InputText("##cli_input", &cli.input,
                         ImGuiInputTextFlags_EnterReturnsTrue
                             | ImGuiInputTextFlags_CallbackHistory
                             | ImGuiInputTextFlags_CallbackCompletion,
                         callback, &cli)) {
        cli_history_push(cli, cli.input);
        fired = run_cli_command(cli.input, deps, cli.scrollback);
        cli.input.clear();
        // Keep the prompt focused after Enter so the operator can keep
        // typing — matches every terminal ever.
        ImGui::SetKeyboardFocusHere(-1);
    }
    return fired;
}

}  // namespace zg::ui
