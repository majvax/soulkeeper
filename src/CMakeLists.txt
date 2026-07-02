# src/CMakeLists.txt


file(GLOB_RECURSE CLIENT_SOURCES
    "client.cpp"
    "core/*.cpp"
    "client/*.cpp"
    "shared/*.cpp"
)

add_executable(client ${CLIENT_SOURCES})

target_link_libraries(client
    PRIVATE
        SDL3::SDL3-shared
        imgui::imgui
        enet
        stb
        sol2::sol2
        fmt::fmt
)

target_include_directories(client
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
)

file(GLOB_RECURSE SERVER_SOURCES
    "server.cpp"
    "core/*.cpp"
    "server/*.cpp"
    "shared/*.cpp"
)

add_executable(server ${SERVER_SOURCES})

target_link_libraries(server
    PRIVATE
    spdlog::spdlog
    enet
    sol2::sol2
    fmt::fmt
)

target_include_directories(server
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
)
