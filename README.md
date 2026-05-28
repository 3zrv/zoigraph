# zoigraph

A native, air-gapped 3D force-directed personal knowledge base and
situational-awareness map. Static nodes persist in a local SQLite database;
ephemeral "phantom" nodes can be injected over a loopback UDP socket and
decay over a TTL. Edges carry a label, a kind, and a certainty that drives
render alpha. Multi-project — each project is its own DB file under
`projects/`.

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
| ESC × 3            | wipe + exit                         |

The control panel has four tabs:

- **Project** — switch / create / delete projects, tag filter (with dim-
  non-matching toggle), auto-cluster, and view flags (grid, CRT shader,
  Barnes-Hut physics). Every checkbox has a hover tooltip.
- **Inspector** — node/edge/phantom/fps stats, FTS5-backed real-time
  search box (jumps camera + selection to the top hit), selected-node
  editor (tier, tags, markdown content with `[[wikilinks]]`), incident-
  edges editor (label / kind / certainty), first-seen + last-touched.
- **Toolbar** — create a static node, inject a phantom locally, save a
  timestamped journal entry (auto-edged from self and from the current
  selection).
- **Help** — terse cheat-sheet of every input listed above.

## Telemetry

A UDP listener is bound to `127.0.0.1:7777` (loopback only). Each datagram
is parsed as one phantom-node JSON payload:

```
{"id": 42, "x": 5.0, "y": 5.0, "z": 5.0, "label": "scan-host-1.2.3.4",
 "connections": [1, 7, 19]}
```

`label` and `connections` are optional. Quick test from the shell:

```
echo -n '{"id":42,"x":5,"y":5,"z":5,"label":"hi"}' \
    > /dev/udp/127.0.0.1/7777
```

The phantom appears as an additive-blended glowing wireframe sphere and
decays to alpha 0 over a 60-second TTL. Click-to-pin promotes it to a
permanent Static Node (writes to the DB, queues into physics, materializes
any `connections` as real edges). Payloads are capped at 1 KiB and over-
sized datagrams are dropped, not parsed-truncated.

## Tests

```
(cd build && ctest --output-on-failure)
```

Fourteen doctest binaries: `forces`, `integrator`, `barnes_hut`,
`graph_buffer`, `picks`, `cluster`, `timeline`, `wikilinks`, `escape_wipe`,
`db`, `project_store`, `seed`, `phantom`, plus a placeholder sanity check.

## Architecture

Three threads, no IPC, no fibers, no third-party concurrency lib:

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

Persistence is plain SQLite (with FTS5) at `projects/<name>.db`. The
persistence layer is structured for a SQLCipher swap-in: the linked
target is named `sqlite3` so the symbol surface stays identical when
AES-256-GCM lands.

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
├── main.cpp                          # ~260-line render loop wiring
├── app/                              # session state, hotkeys, mouse pick
├── graph/                            # types, thread-safe buffer, pure algos
│   ├── graph_buffer.{h,cpp}          # mutex-guarded positions handoff
│   ├── picks.{h,cpp}                 # weakly-connected triple picker
│   ├── cluster.{h,cpp}               # label-propagation
│   ├── timeline.{h,cpp}              # first_seen → 1-D timeline layout
│   └── wikilinks.{h,cpp}             # [[title]] parser
├── input/escape_wipe.{h,cpp}         # triple-ESC state machine
├── macros/                           # operator-triggered camera flies
│   ├── rabbit_hole.{h,cpp}
│   └── bones.{h,cpp}
├── persistence/                      # SQLite + per-project files
│   ├── db.{h,cpp}                    # schema, FTS5 triggers, save/load
│   ├── project_store.{h,cpp}         # list / create / delete projects
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
│   └── sizes.h                       # kNodeRadius / kPhantomRadius
├── telemetry/                        # Thread 3 UDP listener (POSIX + Winsock)
│   ├── phantom*.{h,cpp}              # struct, parser, buffer
│   └── telemetry_thread.{h,cpp}
└── ui/                               # ImGui tab/panel renderers
    ├── project_tab.{h,cpp}
    ├── inspector_tab.{h,cpp}
    ├── toolbar_tab.{h,cpp}
    ├── help_tab.{h,cpp}
    └── bones_panel.{h,cpp}

tests/                                # one doctest binary per .cpp
```
