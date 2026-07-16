cmake_minimum_required(VERSION 3.25)

project(fps
    VERSION 0.1.0
    DESCRIPTION "A small custom C++ game engine and an online FPS built on it"
    LANGUAGES C CXX)  # C is needed for vendored C libraries (glad, later ENet)

# Detect whether we are the top-level project (affects defaults only).
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(FPS_IS_TOP_LEVEL ON)
else()
    set(FPS_IS_TOP_LEVEL OFF)
endif()

option(FPS_BUILD_TESTS "Build unit tests" ${FPS_IS_TOP_LEVEL})
option(FPS_ENABLE_SANITIZERS "Enable Address + UB sanitizers (non-MSVC)" OFF)
option(FPS_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON) # consumed by clang-tidy and IDEs

# Single-config generators default to Debug so a fresh checkout "just works".
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release RelWithDebInfo)
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(CompilerWarnings)
include(Sanitizers)

add_subdirectory(third_party)
add_subdirectory(engine)
add_subdirectory(game)

if(FPS_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
