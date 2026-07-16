# Two engine targets:
#   engine           - headless-safe systems (core, input state, CPU mesh /
#                      camera math; later: scene, physics, networking). The
#                      dedicated server links ONLY this; it must never depend
#                      on SDL/GL/audio.
#   engine_platform  - window/GL/input-device layer (SDL3 + glad + ImGui);
#                      rendering and debug UI. Client-only.

add_library(engine STATIC
    assets/asset_cache.cpp
    assets/asset_cache.h
    assets/gltf_loader.cpp
    assets/gltf_loader.h
    assets/paths.cpp
    assets/paths.h
    core/assert.h
    core/log.cpp
    core/log.h
    core/time.cpp
    core/time.h
    core/version.cpp
    core/version.h
    platform/input.cpp
    platform/input.h
    rendering/camera.cpp
    rendering/camera.h
    rendering/mesh_data.cpp
    rendering/mesh_data.h
    scene/scene.cpp
    scene/scene.h
    scene/transform.cpp
    scene/transform.h
)
add_library(fps::engine ALIAS engine)

# Engine headers are included as "engine/core/log.h" etc., so the include
# root is the repository root.
target_include_directories(engine PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")

target_link_libraries(engine
    PUBLIC glm::glm
    PRIVATE cgltf
)

target_compile_definitions(engine
    PRIVATE ENG_VERSION_STRING="${PROJECT_VERSION}"
    # Assertions are enabled for Debug builds of the engine AND all consumers.
    PUBLIC $<$<CONFIG:Debug>:ENG_ENABLE_ASSERTS>
)

fps_set_warnings(engine)
fps_enable_sanitizers(engine)

add_library(engine_platform STATIC
    debug/imgui_layer.cpp
    debug/imgui_layer.h
    platform/window.cpp
    platform/window.h
    rendering/debug_draw.cpp
    rendering/debug_draw.h
    rendering/gl_util.cpp
    rendering/gl_util.h
    rendering/gpu_mesh.cpp
    rendering/gpu_mesh.h
    rendering/shader.cpp
    rendering/shader.h
    rendering/texture.cpp
    rendering/texture.h
)
add_library(fps::engine_platform ALIAS engine_platform)

target_link_libraries(engine_platform
    PUBLIC fps::engine glad imgui
    PRIVATE SDL3::SDL3
)

fps_set_warnings(engine_platform)
fps_enable_sanitizers(engine_platform)
