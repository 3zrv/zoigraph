# zoigraph

A native 3D force-directed personal knowledge base and situational-awareness
map, designed for local-only (air-gapped) use — at-rest encryption is on the
roadmap (SQLCipher), not yet a delivered guarantee. Static nodes persist in a
local SQLite database;
ephemeral "phantom" nodes can be injected over a loopback UDP socket and
decay over a TTL. Edges carry a label, a kind, and a certainty that drives
render alpha. Multi-project — each project is its own DB file under
`projects/`.

A built-in **LLM bridge** lets a local model (Ollama by default) propose
phantoms about the currently-selected node — a structured trust gradient
(node tier + edge certainty + UDP-only injection) keeps model output
visually marked as provisional until a human pins it. All pin / decay /
edit / delete / bones-throw events are logged into a per-project `events`
table for downstream analysis.

> Work in progress.

## Build

CMake ≥ 3.24 and a C++20 compiler. On Linux you also need X11/Wayland +
OpenGL development headers for raylib:

```
sudo apt install build-essential cmake \
    libgl1-mesa-dev libx11-dev libxrandr-dev libxinerama-dev \
    libxi-dev libxcursor-dev libxext-dev libwayland-dev libxkbcommon-dev
```

macOS and Windows runners need nothing past their default toolchain
(Xcode Command Line Tools / MSVC). Configure and build:

```
cmake -S . -B build
cmake --build build -j$(nproc)        # Linux/macOS
cmake --build build --config Release   # Windows
./build/zoigraph                       # or build\Release\zoigraph.exe
```

The first configure downloads pinned versions of raylib 5.5, Dear ImGui
v1.92.8, rlImGui, the SQLite amalgamation 3.45.2, nlohmann/json 3.11.3,
and doctest v2.4.11 via FetchContent. Two to three minutes online; cached
locally after that.

### Sanitizers

```
cmake -S . -B build-asan -DZG_ASAN=ON
```

Applies AddressSanitizer to every translation unit (the third-party deps
too — partial instrumentation breaks linking).

## Controls

| input              | effect                              |
|--------------------|-------------------------------------|
| left-click a node  | select                              |
| double-click       | select + open the Inspector tab     |
| right-drag         | orbit camera                        |
| shift + right-drag | pan                                 |
| scroll wheel       | zoom                                |
| R                  | reset view                          |
| H                  | rabbit hole (3-hop fly from select) |
| B                  | throw the bones (3 weakly-linked)   |
| T                  | timeline collapse / restore         |
| L                  | toggle all node titles overlay      |
| ESC × 3            | clean exit                          |

Node titles auto-render on the selected node + its 1-hop neighbours +
every phantom's connection targets + the bones triple. Press **L** to
flood the field with every non-deleted title for whole-graph navigation.

The control panel has five tabs:

- **Project** — switch / create / delete projects, tag filter (with dim-
  non-matching toggle), auto-cluster, and view flags (grid, CRT shader,
  Barnes-Hut physics). Every checkbox has a hover tooltip.
- **Inspector** — node/edge/phantom/fps stats, FTS5-backed real-time
  search box (jumps camera + selection to the top hit), selected-node
  editor (tier, tags, markdown content with `[[wikilinks]]`), incident-
  edges editor (label / kind / certainty) plus add-edge-by-title-search,
  first-seen + last-touched.
  **"ask about selection"** fires the configured LLM at the selected
  node and lets the result land as a phantom; **"delete node..."** soft-
  deletes (two-click arm/confirm) so the node vanishes from the field
  without losing the pin trace in the events table.
- **Toolbar** — create a static node (optionally spawned beside and
  edged to the current selection), inject a phantom locally, save a
  timestamped journal entry (auto-edged from self and from the current
  selection).
- **CLI** — claude-style slash-command prompt with scrollback (`/help`
  lists everything; Up/Down recalls history, Tab completes a command,
  `/clear` wipes the pane):
  - `/node "title" [--tier t] [--to ref]` / `/edge <a> <b> [kind]` —
    create nodes and edges; node refs are an id, an exact title, or an
    FTS search term.
  - `/select <ref>` / `/neighbors <ref>` — select + fly to a node; list
    a node's incident edges with kind + certainty.
  - `/tier <ref> <t>` — set a node's tier (confirmed/suspected/phantom).
  - `/delete <ref> [--confirm]` — tombstone a node; the unarmed form
    prints the exact confirming command with the resolved numeric id so
    a fuzzy ref can't delete something other than what it showed.
  - `/projects`, `/project <name> [--create]`, `/info` — list/switch
    projects and show active-project stats.
  - `/search <terms...>` — FTS5 search; lists hits, selects + flies to
    the top one (shares state with the inspector search box).
  - `/phantom "label" [--n N] [--cat c]` — inject a batch of local
    phantoms; `/phantoms [cat]` lists active ones; `/filter <cat|all>`
    hides phantoms outside one category (visual only — hidden phantoms
    still age, decay, and log).
  - `/pin <phantom-id>` / `/decay <phantom-id|all>` — accept (promote
    to a static node, same path + event log as click-to-pin) or dismiss
    without pinning (logs as an ordinary decay at dismissal time).
  - `/ask [ref]` — fire the LLM bridge at a node (defaults to the
    selection); resulting phantoms land via the UDP listener.
  - `/settings`, `/set <grid|crt|dim> <on|off>`, `/set port <n>` (or
    `/port <n>`), `/set size <WxH>` — view, listener, and window-size
    settings, persisted in `settings.json`; a port change tears down and
    rebinds the UDP listener in flight, a size change resizes the window
    live. The window size also follows drag-resizes: the last size is
    captured on clean exit and restored at the next launch.
  - `/panic` overwrites then deletes **all** project data (every
    `projects/*.db` with WAL/SHM sidecars, the `.last` marker,
    `settings.json`, and any leftover legacy `zoigraph.db`) and exits.
    The overwrite-before-unlink is best-effort — CoW filesystems, SSD
    wear-leveling, and journals can retain stale blocks; real at-rest
    protection arrives with SQLCipher.
- **Help** — terse cheat-sheet of every input listed above.

## Telemetry

A UDP listener is bound to `127.0.0.1:7777` (loopback only; port
configurable via `/set port <n>`, persisted in `settings.json`). Each
datagram is parsed as one phantom-node JSON payload:

```
{
  "id": 42,
  "x": 5.0, "y": 5.0, "z": 5.0,
  "label": "scan-host-1.2.3.4",
  "content": "one or two sentences of reasoning",
  "source": "ollama:llama3.2:3b",
  "category": "recon",
  "connections": [
    {"target": 1,  "kind": "saw-at"},
    {"target": 19, "kind": "shell-of"}
  ]
}
```

Every field except `id` / `x` / `y` / `z` is optional. `connections` also
accepts the legacy shape `[1, 7, 19]` (bare ints) — each entry becomes a
connection with empty kind. Quick test from the shell:

```
echo -n '{"id":42,"x":5,"y":5,"z":5,"label":"hi"}' \
    > /dev/udp/127.0.0.1/7777
```

The phantom appears as an additive-blended glowing wireframe sphere and
decays to alpha 0 over a 60-second TTL. Click-to-pin promotes it to a
permanent Static Node at the lowest `tier="phantom"` with edges at
`certainty="phantom"` (visibly faded until the operator promotes them in
the inspector). Payloads are capped at 1 KiB and over-sized datagrams are
dropped, not parsed-truncated.

## LLM bridge

The inspector's **"ask about selection"** button is the in-app path: it
shells out to `scripts/llm_phantom.py emit --anchor-id <id>`, which pulls
the selected node's **relevance neighbourhood** from the running app over
the query channel (see below), builds a prompt from it, and lets the
response land via the normal UDP listener. A 100 ms TCP probe against
`127.0.0.1:11434` runs first so a missing Ollama daemon surfaces a red
error inline rather than hanging. Backend hardcoded to
`ollama:llama3.2:3b` for now.

External agents can drip phantoms from any source:

```
python3 scripts/llm_phantom.py emit \
    --backend ollama --model llama3.2:3b \
    --db projects/<active>.db \
    --anchor-id <node-id>
```

The script enforces a strict JSON schema before sending and tags every
emission with `source="<backend>:<model>"` so the analysis can break pin
rate down by emitter (the ceiling-vs-floor comparison between models).

### Query channel

A second loopback listener — a UDP request/response channel on
`127.0.0.1:7778` (configurable via `query_port` in `settings.json`) — lets
the LLM bridge read the **live** graph instead of opening the SQLite file
directly. Three read queries, one JSON object per datagram:

- `{"q":"neighborhood","id":<id>,"hops":<n>,"token":"…"}` — the anchor plus
  its most relevant neighbours, ranked by **personalized PageRank** (graph
  relevance, not raw spatial proximity), with the edges among them.
- `{"q":"search","text":"…","token":"…"}` — FTS5 hits.
- `{"q":"node","id":<id>,"token":"…"}` — one node.

Replies exclude tombstoned nodes and truncate each node's content so they
fit a single datagram. The render thread answers from its own data once
per frame; the socket thread never touches the graph. Each request must
carry a per-session **token** the app writes (mode `0600`) to
`<db>.db.token` on open — the read channel exposes node content, so an
unauthenticated caller is dropped with no reply. This also keeps the
eventual SQLCipher swap-in clean: the emitter never needs the DB key.
`emit` falls back to a direct DB read when the channel is unreachable (no
running app, or `--no-channel`).

### Events table

Every phantom lifecycle event lands in a per-project `events` table:

| kind             | when                                              |
|------------------|---------------------------------------------------|
| `phantom_spawn`  | UDP packet parsed + added to the buffer           |
| `phantom_pin`    | operator click-to-pin promotes to a Static Node   |
| `phantom_decay`  | TTL expired without a pin                         |
| `node_edit`      | inspector text-edit on title/content              |
| `node_delete`    | inspector soft-delete                             |
| `bones_throw`    | B key triggers a 3-node bones throw               |

Dump and summarise after a session:

```
python3 scripts/export_events.py --db projects/<active>.db
```

Output includes total pin rate, time-to-pin distribution, pin-then-edit-
within-60s rate, per-source breakdown, and a stop-criterion check (pin
rate should sit in [5, 50] for the trust gradient to be doing real
work).

## Tests

```
(cd build && ctest --output-on-failure)
```

Twenty-five doctest binaries: `forces`, `integrator`, `barnes_hut`,
`graph_buffer`, `picks`, `cluster`, `ppr`, `timeline`, `wikilinks`,
`escape_wipe`, `db`, `project_store`, `secure_wipe`, `seed`, `settings`,
`phantom`, `query_protocol`, `query_responder`, `query_token`, `ask`,
`promote`, `phantom_lifecycle`, `labels`, `cli`, plus a placeholder sanity
check.

Pure-logic modules ship with doctest cases before being threaded into
runtime code; render-loop and ImGui-bound code is exempt by design (no
useful unit-test path without a window).

## Architecture

Three render/physics/telemetry threads plus a query-channel listener, no
IPC, no fibers, no third-party concurrency lib:

1. **Main / render** — raylib window, Camera3D, instanced node draw via
   a GLSL 330 vs/fs pair (one draw call for the whole field, split into
   two when a tag filter dims non-matching bodies), CRT post-process
   pipeline (chromatic aberration + scrolling scanlines + vignette over
   an off-screen render texture), ImGui inspector frame layered on top.
2. **Physics** — Coulomb pairwise repulsion + Hooke springs along edges
   + linear centering force, symplectic Euler with damping and a
   velocity clamp, published 120 Hz via a mutex-guarded `GraphBuffer`.
   Toggleable O(N²) ↔ Barnes-Hut octree.
3. **Telemetry** — UDP socket polled on a 100 ms tick (POSIX `poll`,
   `WSAPoll` on Windows), JSON → `Phantom` pushed into a TTL-expiring
   `PhantomBuffer`.
4. **Query channel** — sibling UDP request/response socket on `:7778`
   (same platform shim, `recvfrom`/`sendto`). The socket thread only moves
   bytes through a mailbox; the render thread drains it once per frame and
   answers reads (PPR neighbourhood / FTS search / node) from its own data.

Persistence is plain SQLite (with FTS5) at `projects/<name>.db`. The
schema covers `nodes` (with tier + soft-delete tombstone), `edges`
(with label/kind/certainty), `node_tags`, `meta` (project-level
key/value), and `events` (append-only telemetry log). The persistence
layer is structured for a SQLCipher swap-in: the linked target is
named `sqlite3` so the symbol surface stays identical when AES-256-GCM
lands.

## Platforms

Single binary on each:

- **Linux** — primary development target.
- **macOS** — `cmake -S . -B build && cmake --build build` Just Works.
- **Windows** — same. The telemetry listener uses Winsock2 behind an
  `#ifdef _WIN32`; CMake links `ws2_32` instead of `pthread`.

CI runs the build + test matrix across all three on every push; tagged
releases (`v*`) ship stripped binaries to a GitHub Release. See
[.github/workflows/](.github/workflows/).

## Layout

```
src/
├── main.cpp                          # ~280-line render loop wiring
├── app/                              # session state, hotkeys, render-loop glue
│   ├── session.{h,cpp}               # per-project Session struct + open_project
│   ├── hotkeys.{h,cpp}               # ESC/H/B/T/L key handlers
│   ├── pick.{h,cpp}                  # mouse raypick + click-to-pin
│   ├── promote.{h,cpp}               # pure phantom→StoredNode promotion (tested)
│   ├── phantom_lifecycle.{h,cpp}     # per-frame spawn/decay diff (tested)
│   ├── settings.{h,cpp}              # persisted operator settings (tested)
│   ├── ask.{h,cpp}                   # LLM Ask button: TCP probe + popen
│   ├── query_responder.{h,cpp}       # answer channel reads from the live graph (tested)
│   └── query_token.{h,cpp}           # per-session 0600 auth token (tested)
├── graph/                            # types, thread-safe buffer, pure algos
│   ├── graph_buffer.{h,cpp}          # mutex-guarded positions handoff
│   ├── picks.{h,cpp}                 # weakly-connected triple picker
│   ├── cluster.{h,cpp}               # label-propagation
│   ├── ppr.{h,cpp}                   # personalized PageRank for relevance (tested)
│   ├── timeline.{h,cpp}              # first_seen → y-z spiral disc layout
│   └── wikilinks.{h,cpp}             # [[title]] parser
├── input/escape_wipe.{h,cpp}         # triple-ESC-to-exit state machine
├── macros/                           # operator-triggered camera flies
│   ├── rabbit_hole.{h,cpp}
│   └── bones.{h,cpp}
├── persistence/                      # SQLite + per-project files
│   ├── db.{h,cpp}                    # schema, FTS5 triggers, events log
│   ├── project_store.{h,cpp}         # list / create / delete projects
│   ├── secure_wipe.{h,cpp}           # /panic overwrite-then-unlink wipe
│   └── seed.{h,cpp}                  # fresh-project seed graph
├── physics/
│   ├── forces.{h,cpp}                # pure Coulomb + Hooke (unit tested)
│   ├── barnes_hut.{h,cpp}            # octree repulsion (unit tested)
│   └── physics_thread.{h,cpp}        # integrate_step + Thread 2 orchestration
├── render/                           # GPU + 2D overlay code
│   ├── shaders.h                     # GLSL 330 instancing + CRT fragment
│   ├── imgui_theme.{h,cpp}
│   ├── camera.{h,cpp}                # orbit camera + defaults
│   ├── draw.{h,cpp}                  # draw_jagged_line + small helpers
│   ├── scene.{h,cpp}                 # full 3D pass (bodies/edges/halos)
│   ├── composite.{h,cpp}             # CRT composite + edge labels + ESC HUD
│   ├── labels.{h,cpp}                # in-focus title set + DrawText overlay
│   └── sizes.h                       # kNodeRadius / kPhantomRadius
├── telemetry/                        # Thread 3 UDP listener (POSIX + Winsock)
│   ├── phantom.{h,cpp}               # Phantom + Connection structs
│   ├── phantom_parse.{h,cpp}         # JSON → Phantom (both connection shapes)
│   ├── phantom_buffer.{h,cpp}        # TTL-expiring shared buffer
│   ├── telemetry_thread.{h,cpp}      # Thread 3 UDP phantom listener
│   ├── query_protocol.{h,cpp}        # query-channel wire format (tested)
│   └── query_thread.{h,cpp}          # :7778 request/response socket + mailbox
└── ui/                               # ImGui tab/panel renderers
    ├── project_tab.{h,cpp}
    ├── inspector_tab.{h,cpp}
    ├── toolbar_tab.{h,cpp}
    ├── cli_tab.{h,cpp}               # slash-command prompt (/panic, /help)
    ├── help_tab.{h,cpp}
    └── bones_panel.{h,cpp}

scripts/                              # external bridge + analysis
├── llm_phantom.py                    # backend-agnostic emit + measure harness
├── seed_corpus_unix.py               # UNIX-history sample corpus seeder
└── export_events.py                  # events table → CSV + pin-rate summary

tests/                                # one doctest binary per .cpp
```
