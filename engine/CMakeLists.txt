# Two engine targets:
#   engine           - headless-safe systems (core, input state, later: scene,
#                      physics, networking). The dedicated server links ONLY
#                      this; it must never depend on SDL/GL/audio.
#   engine_platform  - window/GL/input-device layer (SDL3 + glad); later also
#                      rendering, audio, debug UI. Client-only.

add_library(engine STATIC
    core/assert.h
    core/log.cpp
    core/log.h
    core/time.cpp
    core/time.h
    core/version.cpp
    core/version.h
    platform/input.cpp
    platform/input.h
)
add_library(fps::engine ALIAS engine)

# Engine headers are included as "engine/core/log.h" etc., so the include
# root is the repository root.
target_include_directories(engine PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")

target_compile_definitions(engine
    PRIVATE ENG_VERSION_STRING="${PROJECT_VERSION}"
    # Assertions are enabled for Debug builds of the engine AND all consumers.
    PUBLIC $<$<CONFIG:Debug>:ENG_ENABLE_ASSERTS>
)

fps_set_warnings(engine)
fps_enable_sanitizers(engine)

add_library(engine_platform STATIC
    platform/window.cpp
    platform/window.h
)
add_library(fps::engine_platform ALIAS engine_platform)

target_link_libraries(engine_platform
    PUBLIC fps::engine glad
    PRIVATE SDL3::SDL3
)

fps_set_warnings(engine_platform)
fps_enable_sanitizers(engine_platform)
