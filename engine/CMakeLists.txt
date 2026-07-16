add_library(engine STATIC
    core/assert.h
    core/log.cpp
    core/log.h
    core/version.cpp
    core/version.h
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
