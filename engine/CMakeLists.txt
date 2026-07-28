# Two engine targets:
#   engine           - headless-safe systems (core, input state, CPU mesh /
#                      camera math; later: scene, physics, networking). The
#                      dedicated server links ONLY this; it must never depend
#                      on SDL/GL/audio.
#   engine_platform  - window/GL/input-device layer (SDL3 + glad + ImGui);
#                      rendering and debug UI. Client-only.

add_library(engine STATIC
    animation/skeleton.cpp
    animation/skeleton.h
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
    net/byte_buffer.cpp
    net/byte_buffer.h
    net/client_transport.cpp
    net/client_transport.h
    net/composite_transport.cpp
    net/composite_transport.h
    net/transport.cpp
    net/transport.h
    net/websocket_host.cpp
    net/websocket_host.h
    physics/character_controller.cpp
    physics/character_controller.h
    physics/physics_internal.h
    physics/physics_world.cpp
    physics/physics_world.h
    platform/input.cpp
    platform/input.h
    rendering/camera.cpp
    rendering/camera.h
    rendering/light.cpp
    rendering/light.h
    rendering/mesh_data.cpp
    rendering/mesh_data.h
    rendering/particle_sim.cpp
    rendering/particle_sim.h
    rendering/postfx_math.cpp
    rendering/postfx_math.h
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
    PRIVATE cgltf stb Jolt enet
)
if(WIN32)
    # WebSocketHost uses Winsock directly.
    target_link_libraries(engine PRIVATE ws2_32)
endif()

target_compile_definitions(engine
    PRIVATE ENG_VERSION_STRING="${PROJECT_VERSION}"
    # Assertions are enabled for Debug builds of the engine AND all consumers.
    PUBLIC $<$<CONFIG:Debug>:ENG_ENABLE_ASSERTS>
)

if(FPS_ENABLE_WEBRTC)
    target_sources(engine PRIVATE net/webrtc_host.cpp net/webrtc_host.h)
    target_link_libraries(engine PRIVATE LibDataChannel::LibDataChannelStatic)
    target_compile_definitions(engine PUBLIC FPS_HAS_WEBRTC)
endif()

fps_set_warnings(engine)
fps_enable_sanitizers(engine)

# Client-only. A dedicated-server build (FPS_BUILD_CLIENT=OFF) never defines
# this target, and none of its dependencies are even fetched -- see the note in
# third_party/CMakeLists.txt.
if(NOT FPS_BUILD_CLIENT)
    return()
endif()

add_library(engine_platform STATIC
    audio/audio_engine.cpp
    audio/audio_engine.h
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
    rendering/joint_texture.cpp
    rendering/joint_texture.h
    rendering/particle_renderer.cpp
    rendering/particle_renderer.h
    rendering/postfx.cpp
    rendering/postfx.h
    rendering/render_target.cpp
    rendering/render_target.h
    rendering/screenshot.cpp
    rendering/screenshot.h
    rendering/shader.cpp
    rendering/shader.h
    rendering/shadow_map.cpp
    rendering/shadow_map.h
    rendering/texture.cpp
    rendering/texture.h
)
add_library(fps::engine_platform ALIAS engine_platform)

target_link_libraries(engine_platform
    PUBLIC fps::engine imgui
    PRIVATE SDL3::SDL3 miniaudio stb
)
if(NOT EMSCRIPTEN)
    # glad loads desktop GL entry points; WebGL functions come from the
    # browser, so the loader is native-only.
    target_link_libraries(engine_platform PUBLIC glad)
endif()

fps_set_warnings(engine_platform)
fps_enable_sanitizers(engine_platform)
