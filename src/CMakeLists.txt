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

# C++26 reflection (P2996) + contracts (P2900): the reflective component
# registration (shared/mod) and the ECS pre() contracts need them. Project
# targets only — deps never include the headers that use these features.
# Contracts default to the enforce semantic; violations print function,
# file:line and the predicate, then terminate.
foreach(tgt client server)
    target_compile_options(${tgt} PRIVATE
        $<$<CXX_COMPILER_ID:GNU>:-freflection -fcontracts>
    )
    # -fcontracts at link time too: the driver pulls in the default
    # contract-violation handler (prints the predicate, terminates).
    target_link_options(${tgt} PRIVATE
        $<$<CXX_COMPILER_ID:GNU>:-fcontracts>
    )
endforeach()
