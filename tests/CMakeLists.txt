include(FetchContent)

FetchContent_Declare(
    Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG v3.8.0
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(Catch2)

add_executable(engine_tests
    animation/skeleton_tests.cpp
    assets/gltf_loader_tests.cpp
    assets/paths_tests.cpp
    core/input_tests.cpp
    core/log_tests.cpp
    core/time_tests.cpp
    core/version_tests.cpp
    game/arsenal_tests.cpp
    game/bot_tests.cpp
    game/hitscan_tests.cpp
    game/interpolation_tests.cpp
    game/kill_cam_tests.cpp
    game/lag_comp_tests.cpp
    game/player_color_tests.cpp
    game/player_movement_tests.cpp
    game/prediction_tests.cpp
    game/replay_tests.cpp
    game/weapon_tests.cpp
    net/byte_buffer_tests.cpp
    net/composite_transport_tests.cpp
    net/protocol_tests.cpp
    net/transport_tests.cpp
    net/websocket_host_tests.cpp
    rendering/camera_tests.cpp
    rendering/light_tests.cpp
    rendering/mesh_data_tests.cpp
    rendering/particle_tests.cpp
    rendering/postfx_tests.cpp
    scene/scene_tests.cpp
)

# The server library is native-only (see game/CMakeLists.txt), so anything
# testing it has to be too. stats_store.cpp used to be compiled in here
# directly as a one-off because the server was an executable; now that it is
# a library, it links like anything else.
#
# game_server pulls in fps::engine and fps::game_shared PUBLICly, so naming
# them again here would put each archive on the link line twice (ld warns).
# Under Emscripten there is no game_server, so they are named directly.
if(NOT EMSCRIPTEN)
    target_sources(engine_tests PRIVATE
        server/server_game_tests.cpp
        server/stats_store_tests.cpp
    )
    target_link_libraries(engine_tests PRIVATE fps::game_server)
else()
    target_link_libraries(engine_tests PRIVATE fps::engine fps::game_shared)
endif()
# The WebRTC tests drive libdatachannel directly as the client half, so they
# only exist when the transport is built.
if(FPS_ENABLE_WEBRTC)
    target_sources(engine_tests PRIVATE net/webrtc_host_tests.cpp)
    target_link_libraries(engine_tests PRIVATE LibDataChannel::LibDataChannelStatic)
endif()

target_link_libraries(engine_tests PRIVATE Catch2::Catch2WithMain)
fps_set_warnings(engine_tests)
fps_enable_sanitizers(engine_tests)

list(APPEND CMAKE_MODULE_PATH "${catch2_SOURCE_DIR}/extras")
include(Catch)
catch_discover_tests(engine_tests)
