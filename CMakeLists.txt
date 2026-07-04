cmake_minimum_required(VERSION 4.3)
include(cmake/warning.cmake)

# Default to an optimized build: the Lua/sol2 sim layer is 10-50x slower at
# -O0, which alone blows the 120 Hz tick budget at a few hundred entities.
if (NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

if (NOT DEFINED CMAKE_CXX_STANDARD)
  set(CMAKE_CXX_STANDARD 26)
endif()

set(CMAKE_CXX_EXTENSIONS OFF)


project(
    project
    VERSION 0.0.1
    DESCRIPTION ""
    HOMEPAGE_URL ""
    LANGUAGES C CXX
)


set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin)

# Explicitly disable automatic C++20 module scanning to avoid injecting
# unsupported experimental flags like -fmodules-ts with some GCC/MinGW builds.
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

include(cmake/version.cmake)
include(Dependencies.cmake)

# Add source tree
add_subdirectory(src)
