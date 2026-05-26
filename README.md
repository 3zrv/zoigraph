# zoigraph

A native, air-gapped 3D force-directed personal knowledge base and
situational-awareness map — the "Obsidian-but-evil" red-string corkboard from
[System Directive ZOIGRAPH](init.md).

> Work in progress. The architecture is in place; several directive features
> are still pending. See git log and `init.md` for what's done and what's not.

## Build (Linux)

CMake ≥ 3.24 and a C++20 compiler. Raylib needs X11/GL development headers:

```
sudo apt install build-essential cmake \
    libgl1-mesa-dev libx11-dev libxrandr-dev libxinerama-dev \
    libxi-dev libxcursor-dev libxext-dev libwayland-dev libxkbcommon-dev
```

Configure and build:

```
cmake -S . -B build
cmake --build build -j$(nproc)
./build/zoigraph
```

The first configure downloads pinned versions of raylib 5.5, Dear ImGui
v1.92.8, rlImGui, the SQLite amalgamation 3.45.2, nlohmann/json 3.11.3, and
doctest v2.4.11 via FetchContent. Two to three minutes online; cached after
that.

## Controls

| input              | effect              |
|--------------------|---------------------|
| left-click a node  | select              |
| right-drag         | orbit camera        |
| shift + right-drag | pan                 |
| scroll wheel       | zoom                |
| R                  | reset view          |

The inspector panel shows live node / edge / phantom counts, FPS, the
selected node's editable title and markdown content, an FTS5-backed
real-time search box, and the control glossary.

## Telemetry

A UDP listener is bound to `127.0.0.1:7777` (loopback only — air-gapped).
Each datagram is parsed as one phantom-node JSON payload:

```
{"id": 42, "x": 5.0, "y": 5.0, "z": 5.0, "label": "scan-host-1.2.3.4"}
```

`label` is optional. Quick test from the shell:

```
echo -n '{"id":42,"x":5,"y":5,"z":5,"label":"hi"}' \
    > /dev/udp/127.0.0.1/7777
```

The phantom appears as a glowing yellow wireframe sphere and decays to
alpha 0 over the 60-second TTL from directive §5.B.

## Tests

```
(cd build && ctest --output-on-failure)
```

Six doctest binaries: `forces`, `integrator`, `graph_buffer`, `db`,
`phantom`, plus a placeholder sanity check.

## Architecture

Three threads, no IPC, no fibers, no third-party concurrency lib:

1. **Main / render** — raylib window, Camera3D, instanced node draw via a
   GLSL 330 vs/fs pair, ImGui inspector frame.
2. **Physics** — Coulomb pairwise repulsion + Hooke springs along edges +
   linear centering force, symplectic Euler with damping and a velocity
   clamp, published 120 Hz via a mutex-guarded `GraphBuffer`.
3. **Telemetry** — UDP socket polled on a 100 ms tick, JSON → `Phantom`
   pushed into a TTL-expiring `PhantomBuffer`.

Persistence is plain SQLite (with FTS5) at `./zoigraph.db`. The persistence
layer is structured for a SQLCipher swap-in: the linked target is named
`sqlite3` so the symbol surface stays identical when AES-256-GCM lands.

## Layout

```
src/
├── main.cpp                       # render thread + ImGui inspector
├── graph/                         # Node / Edge types, thread-safe buffer
├── physics/
│   ├── forces.{h,cpp}             # pure Coulomb + Hooke (unit tested)
│   └── physics_thread.{h,cpp}     # integrate_step + Thread 2 orchestration
├── persistence/
│   └── db.{h,cpp}                 # SQLite wrapper, FTS5 schema + triggers
└── telemetry/
    ├── phantom*.{h,cpp}           # parser + buffer (unit tested)
    └── telemetry_thread.{h,cpp}   # Thread 3 UDP listener

tests/
├── test_forces.cpp
├── test_integrator.cpp
├── test_graph_buffer.cpp
├── test_db.cpp
└── test_phantom.cpp
```
