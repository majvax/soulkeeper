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
