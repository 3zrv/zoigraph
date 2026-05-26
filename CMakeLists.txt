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
)
target_include_directories(imgui PUBLIC ${imgui_src_SOURCE_DIR})

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
add_library(zg_physics STATIC src/physics/forces.cpp)
target_include_directories(zg_physics PUBLIC src)
target_link_libraries(zg_physics PUBLIC raylib)

# ---- application -----------------------------------------------------------
add_executable(zoigraph src/main.cpp)
target_link_libraries(zoigraph PRIVATE
    zg_physics
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
