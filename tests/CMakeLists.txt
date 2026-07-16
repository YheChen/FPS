include(FetchContent)

FetchContent_Declare(
    Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG v3.8.0
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(Catch2)

add_executable(engine_tests
    assets/gltf_loader_tests.cpp
    assets/paths_tests.cpp
    core/input_tests.cpp
    core/log_tests.cpp
    core/time_tests.cpp
    core/version_tests.cpp
    game/hitscan_tests.cpp
    game/interpolation_tests.cpp
    game/player_movement_tests.cpp
    game/prediction_tests.cpp
    game/weapon_tests.cpp
    net/byte_buffer_tests.cpp
    net/protocol_tests.cpp
    net/transport_tests.cpp
    rendering/camera_tests.cpp
    rendering/mesh_data_tests.cpp
    scene/scene_tests.cpp
)
target_link_libraries(engine_tests PRIVATE fps::engine fps::game_shared Catch2::Catch2WithMain)
fps_set_warnings(engine_tests)
fps_enable_sanitizers(engine_tests)

list(APPEND CMAKE_MODULE_PATH "${catch2_SOURCE_DIR}/extras")
include(Catch)
catch_discover_tests(engine_tests)
