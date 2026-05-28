cmake_minimum_required(VERSION 3.24)
project(zoigraph LANGUAGES CXX C)

# CMake 4 removed compatibility with policies < 3.5. Some upstream deps still
# declare `cmake_minimum_required(VERSION 3.0)`; this lets them configure.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "" FORCE)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# Opt-in AddressSanitizer build: `cmake -S . -B build -DZG_ASAN=ON`
# Applies globally so every TU (including FetchContent deps and tests) shares
# the same ASAN runtime — partial instrumentation breaks linking.
option(ZG_ASAN "Build zoigraph with AddressSanitizer" OFF)
if(ZG_ASAN)
    add_compile_options(-fsanitize=address -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=address)
endif()

include(FetchContent)
set(FETCHCONTENT_QUIET OFF)

# ---- raylib ----------------------------------------------------------------
set(BUILD_EXAMPLES   OFF CACHE BOOL "" FORCE)
set(BUILD_GAMES      OFF CACHE BOOL "" FORCE)
set(RAYLIB_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    raylib
    GIT_REPOSITORY https://github.com/raysan5/raylib.git
    GIT_TAG        5.5
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(raylib)

# ---- Dear ImGui (no upstream CMake; build as static lib) -------------------
FetchContent_Declare(
    imgui_src
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        v1.92.8
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(imgui_src)
add_library(imgui STATIC
    ${imgui_src_SOURCE_DIR}/imgui.cpp
    ${imgui_src_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_src_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_src_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_src_SOURCE_DIR}/misc/cpp/imgui_stdlib.cpp
)
target_include_directories(imgui PUBLIC
    ${imgui_src_SOURCE_DIR}
    ${imgui_src_SOURCE_DIR}/misc/cpp
)

# ---- rlImGui (raylib ImGui backend) ----------------------------------------
FetchContent_Declare(
    rlimgui_src
    GIT_REPOSITORY https://github.com/raylib-extras/rlImGui.git
    GIT_TAG        main
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(rlimgui_src)
add_library(rlimgui STATIC ${rlimgui_src_SOURCE_DIR}/rlImGui.cpp)
target_include_directories(rlimgui PUBLIC ${rlimgui_src_SOURCE_DIR})
target_link_libraries(rlimgui PUBLIC imgui raylib)

# ---- SQLite (amalgamation; SQLCipher swap-in deferred) ---------------------
FetchContent_Declare(
    sqlite_amalgamation
    URL https://www.sqlite.org/2024/sqlite-amalgamation-3450200.zip
)
FetchContent_MakeAvailable(sqlite_amalgamation)
add_library(sqlite3 STATIC ${sqlite_amalgamation_SOURCE_DIR}/sqlite3.c)
target_include_directories(sqlite3 PUBLIC ${sqlite_amalgamation_SOURCE_DIR})
target_compile_definitions(sqlite3 PUBLIC
    SQLITE_ENABLE_FTS5
    SQLITE_THREADSAFE=1
)
if(NOT WIN32)
    target_link_libraries(sqlite3 PUBLIC ${CMAKE_DL_LIBS} pthread)
endif()

# ---- nlohmann/json (header-only) -------------------------------------------
FetchContent_Declare(
    nlohmann_json
    URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
)
FetchContent_MakeAvailable(nlohmann_json)

# ---- doctest (unit tests) --------------------------------------------------
FetchContent_Declare(
    doctest
    GIT_REPOSITORY https://github.com/doctest/doctest.git
    GIT_TAG        v2.4.11
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(doctest)

# ---- physics (pure functions, separately testable) -------------------------
add_library(zg_physics STATIC
    src/physics/forces.cpp
    src/physics/barnes_hut.cpp
)
target_include_directories(zg_physics PUBLIC src)
target_link_libraries(zg_physics PUBLIC raylib)

# ---- app (per-project session state + open_project + render-loop glue) -----
add_library(zg_app STATIC
    src/app/session.cpp
    src/app/hotkeys.cpp
    src/app/pick.cpp
)
target_include_directories(zg_app PUBLIC src)
target_link_libraries(zg_app PUBLIC
    zg_persistence zg_physics_thread zg_graph zg_telemetry
    zg_input zg_macros zg_render imgui)

# ---- ui (ImGui tab renderers) ----------------------------------------------
add_library(zg_ui STATIC
    src/ui/toolbar_tab.cpp
    src/ui/inspector_tab.cpp
    src/ui/help_tab.cpp
    src/ui/bones_panel.cpp
    src/ui/project_tab.cpp
)
target_include_directories(zg_ui PUBLIC src)
target_link_libraries(zg_ui PUBLIC zg_app zg_telemetry zg_macros zg_graph zg_persistence imgui raylib)

# ---- macros (operator-triggered camera flies: rabbit hole, throw bones) ----
add_library(zg_macros STATIC
    src/macros/rabbit_hole.cpp
    src/macros/bones.cpp
)
target_include_directories(zg_macros PUBLIC src)
target_link_libraries(zg_macros PUBLIC zg_graph raylib)

# ---- render (shaders + imgui theme + camera + scene + composite) -----------
add_library(zg_render STATIC
    src/render/imgui_theme.cpp
    src/render/camera.cpp
    src/render/draw.cpp
    src/render/scene.cpp
    src/render/composite.cpp
)
target_include_directories(zg_render PUBLIC src)
# scene + composite read from Session, Bones, EscapeWipe — pull in the struct
# definitions via zg_graph / zg_macros / zg_input. zg_app would be the natural
# umbrella but zg_app already links zg_render (hotkeys calls update_orbit_camera),
# so depending on zg_app here would loop. The leaf libs are enough — we never
# call into zg_app code.
target_link_libraries(zg_render PUBLIC imgui raylib zg_graph zg_macros zg_input)

# ---- input (pure key/event state machines, testable without raylib) --------
add_library(zg_input STATIC src/input/escape_wipe.cpp)
target_include_directories(zg_input PUBLIC src)

# ---- graph (thread-safe buffer between physics and render) -----------------
add_library(zg_graph STATIC
    src/graph/graph_buffer.cpp
    src/graph/picks.cpp
    src/graph/wikilinks.cpp
    src/graph/cluster.cpp
    src/graph/timeline.cpp
)
target_include_directories(zg_graph PUBLIC src)
target_link_libraries(zg_graph PUBLIC raylib)

# ---- physics simulation thread ---------------------------------------------
# Depends on zg_telemetry because the physics step now snapshots PhantomBuffer
# to apply phantom repulsion against static nodes (directive §5.B).
add_library(zg_physics_thread STATIC src/physics/physics_thread.cpp)
target_include_directories(zg_physics_thread PUBLIC src)
target_link_libraries(zg_physics_thread PUBLIC zg_physics zg_graph zg_telemetry raylib)
if(NOT WIN32)
    target_link_libraries(zg_physics_thread PUBLIC pthread)
endif()

# ---- persistence (plain SQLite for now; SQLCipher swap-in later) -----------
add_library(zg_persistence STATIC
    src/persistence/db.cpp
    src/persistence/project_store.cpp
    src/persistence/seed.cpp
)
target_include_directories(zg_persistence PUBLIC src)
target_link_libraries(zg_persistence PUBLIC zg_graph sqlite3 raylib)

# ---- telemetry (Thread 3: UDP listener + phantom buffer) -------------------
add_library(zg_telemetry STATIC
    src/telemetry/phantom_buffer.cpp
    src/telemetry/phantom_parse.cpp
    src/telemetry/telemetry_thread.cpp
)
target_include_directories(zg_telemetry PUBLIC src)
target_link_libraries(zg_telemetry PUBLIC nlohmann_json::nlohmann_json raylib)
if(WIN32)
    # Winsock2 (sockets) + Ws2_32 (WSAStartup, WSAPoll, etc.) live in the
    # ws2_32 import lib. No DLL deployment needed — it's a Windows system lib.
    target_link_libraries(zg_telemetry PUBLIC ws2_32)
else()
    target_link_libraries(zg_telemetry PUBLIC pthread)
endif()

# ---- application -----------------------------------------------------------
add_executable(zoigraph src/main.cpp)
target_link_libraries(zoigraph PRIVATE
    zg_physics
    zg_graph
    zg_physics_thread
    zg_persistence
    zg_telemetry
    zg_input
    zg_render
    zg_macros
    zg_app
    zg_ui
    raylib
    rlimgui
    imgui
    sqlite3
    nlohmann_json::nlohmann_json
)
target_include_directories(zoigraph PRIVATE src)
if(NOT WIN32)
    target_link_libraries(zoigraph PRIVATE pthread)
endif()


# ---- tests -----------------------------------------------------------------
enable_testing()
add_subdirectory(tests)
