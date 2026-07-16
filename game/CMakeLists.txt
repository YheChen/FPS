# Simulation code shared verbatim by client (prediction) and server
# (authority). Headless-safe: links engine core only, never SDL/GL.
add_library(game_shared STATIC
    shared/health.h
    shared/hitscan.h
    shared/input_command.h
    shared/player_movement.cpp
    shared/player_movement.h
    shared/weapon.cpp
    shared/weapon.h
)
add_library(fps::game_shared ALIAS game_shared)
target_link_libraries(game_shared PUBLIC fps::engine)
fps_set_warnings(game_shared)
fps_enable_sanitizers(game_shared)

add_executable(fps_client
    client/main.cpp
)
target_link_libraries(fps_client PRIVATE fps::engine_platform fps::game_shared)
fps_set_warnings(fps_client)
fps_enable_sanitizers(fps_client)

# The dedicated server stays headless: engine core only, no SDL/GL.
add_executable(fps_server
    server/main.cpp
)
target_link_libraries(fps_server PRIVATE fps::engine fps::game_shared)
fps_set_warnings(fps_server)
fps_enable_sanitizers(fps_server)
