include(cmake/CPM.cmake)



cpmaddpackage("gh:fmtlib/fmt#11.2.0")
cpmaddpackage(URI "gh:gabime/spdlog@1.15.3" OPTIONS "SPDLOG_FMT_EXTERNAL ON")
set(SDL_INSTALL OFF CACHE BOOL "Disable SDL3 install targets" FORCE)
# Dependencies.cmake

CPMAddPackage(
    NAME SDL3
    GITHUB_REPOSITORY libsdl-org/SDL
    GIT_TAG release-3.4.10   # <-- Pinning to a specific, stable snapshot
    EXCLUDE_FROM_ALL YES
)

CPMAddPackage(
    NAME imgui
    GITHUB_REPOSITORY ocornut/imgui
    GIT_TAG docking # Use docking branch for movable debug windows
    DOWNLOAD_ONLY YES # Dear ImGui ships no CMakeLists; we build the lib ourselves
)

# Dear ImGui has no build system of its own. Compile the core plus the SDL3
# renderer backend into a single static library and expose its headers.
if(imgui_ADDED)
    add_library(imgui STATIC
        ${imgui_SOURCE_DIR}/imgui.cpp
        ${imgui_SOURCE_DIR}/imgui_draw.cpp
        ${imgui_SOURCE_DIR}/imgui_tables.cpp
        ${imgui_SOURCE_DIR}/imgui_widgets.cpp
        ${imgui_SOURCE_DIR}/imgui_demo.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_sdlrenderer3.cpp
    )
    target_include_directories(imgui SYSTEM PUBLIC
        ${imgui_SOURCE_DIR}
        ${imgui_SOURCE_DIR}/backends
    )
    target_link_libraries(imgui PUBLIC SDL3::SDL3-shared)
    add_library(imgui::imgui ALIAS imgui)
endif()

CPMAddPackage(
    NAME enet
    GITHUB_REPOSITORY lsalzman/enet
    GIT_TAG master
)

# ENet's CMake uses directory-scoped include_directories(), so the `enet` target
# doesn't export its headers to consumers. Attach them to the target ourselves.
if(enet_ADDED)
    target_include_directories(enet SYSTEM PUBLIC ${enet_SOURCE_DIR}/include)
endif()

# stb (single-header image loader) for PNG sprite loading on the client. No build
# system — just expose the headers via an INTERFACE target.
CPMAddPackage(
    NAME stb
    GITHUB_REPOSITORY nothings/stb
    GIT_TAG master
    DOWNLOAD_ONLY YES
)
if(stb_ADDED)
    add_library(stb INTERFACE)
    target_include_directories(stb SYSTEM INTERFACE ${stb_SOURCE_DIR})
endif()

# Lua 5.4 — the embedded scripting runtime for the modding layer. The official
# mirror ships sources at the repo root but no build system; compile the library
# ourselves (excluding the two standalone-tool mains lua.c / luac.c).
CPMAddPackage(
    NAME lua
    GITHUB_REPOSITORY lua/lua
    GIT_TAG v5.4.7
    DOWNLOAD_ONLY YES
)
if(lua_ADDED)
    file(GLOB LUA_SOURCES "${lua_SOURCE_DIR}/*.c")
    # Drop the CLI (lua.c) and compiler (luac.c) mains, and the onelua.c
    # amalgamation (it re-#includes every unit + a main() -> duplicate symbols).
    list(FILTER LUA_SOURCES EXCLUDE REGEX "/(lua|luac|onelua)\\.c$")
    add_library(lua STATIC ${LUA_SOURCES})
    # Build the C sources as C (they are not C++), and enable POSIX + dlopen on unix.
    set_target_properties(lua PROPERTIES LINKER_LANGUAGE C)
    target_include_directories(lua SYSTEM PUBLIC ${lua_SOURCE_DIR})
    if(UNIX)
        target_compile_definitions(lua PRIVATE LUA_USE_LINUX)
        target_link_libraries(lua PUBLIC m ${CMAKE_DL_LIBS})
    endif()
    add_library(lua::lua ALIAS lua)
endif()

# sol2 — modern C++ <-> Lua binding (header-only). Catch mod errors via safe
# function calls so a broken plugin can never crash the authoritative server.
# No sol2 release since v3.3.0 (2022), which predates GCC 16 / C++26 and
# misdetects Lua 5.4; the actively-maintained `develop` branch is the supported
# choice for modern toolchains.
CPMAddPackage(
    NAME sol2
    GITHUB_REPOSITORY ThePhD/sol2
    GIT_TAG develop
    DOWNLOAD_ONLY YES
)
if(sol2_ADDED)
    add_library(sol2 INTERFACE)
    target_include_directories(sol2 SYSTEM INTERFACE ${sol2_SOURCE_DIR}/include)
    # SOL_SAFE_FUNCTION: mod-callback errors are caught, not UB.
    # SOL_NO_LUA_HPP: force sol2 to include our vendored <lua.h> (5.4) instead of
    # a stray system-installed <lua.hpp> of a different version.
    target_compile_definitions(sol2 INTERFACE SOL_SAFE_FUNCTION=1 SOL_NO_LUA_HPP=1)
    target_link_libraries(sol2 INTERFACE lua::lua)
    add_library(sol2::sol2 ALIAS sol2)
endif()
