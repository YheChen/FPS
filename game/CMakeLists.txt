# Simulation code shared verbatim by client (prediction) and server
# (authority). Headless-safe: links engine core only, never SDL/GL.
add_library(game_shared STATIC
    shared/bot.cpp
    shared/bot.h
    shared/health.h
    shared/hitscan.h
    shared/input_command.h
    shared/interpolation.cpp
    shared/interpolation.h
    shared/player_movement.cpp
    shared/player_movement.h
    shared/prediction.cpp
    shared/prediction.h
    shared/protocol.cpp
    shared/protocol.h
    shared/replay.cpp
    shared/replay.h
    shared/weapon.cpp
    shared/weapon.h
)
add_library(fps::game_shared ALIAS game_shared)
target_link_libraries(game_shared PUBLIC fps::engine)
fps_set_warnings(game_shared)
fps_enable_sanitizers(game_shared)

# The graphical client. Skipped for a dedicated-server build, which does not
# have engine_platform at all (see engine/CMakeLists.txt).
if(FPS_BUILD_CLIENT)
add_executable(fps_client
    client/main.cpp
    client/net_client.cpp
    client/net_client.h
)
target_link_libraries(fps_client PRIVATE fps::engine_platform fps::game_shared)
fps_set_warnings(fps_client)
fps_enable_sanitizers(fps_client)

if(EMSCRIPTEN)
    # WebAssembly client: WebGL2 (GLES3), grow memory, exceptions (Jolt),
    # and bake the assets directory into the virtual filesystem at /assets
    # (find_assets_root() locates it from the MEMFS root cwd).
    # Note: file-path options are separate list items (not "SHELL:-x path")
    # so CMake quotes paths that contain spaces; the repo path may have them.
    target_link_options(fps_client PRIVATE
        "SHELL:-s MAX_WEBGL_VERSION=2"
        "SHELL:-s MIN_WEBGL_VERSION=2"
        "SHELL:-s FULL_ES3=1"
        "SHELL:-s ALLOW_MEMORY_GROWTH=1"
        "SHELL:-s INITIAL_MEMORY=134217728"
        "SHELL:-s STACK_SIZE=4MB"
        "SHELL:-s EXIT_RUNTIME=0"
        "SHELL:-s DISABLE_EXCEPTION_CATCHING=0"
        "SHELL:-l websocket.js"
        --preload-file "${CMAKE_SOURCE_DIR}/assets@/assets"
        --shell-file "${CMAKE_SOURCE_DIR}/web/shell.html"
    )
    target_compile_options(fps_client PRIVATE -fexceptions)
    # Deployed builds bake in their server URL (docs/deploy.md):
    #   emcmake cmake --preset web -DFPS_WEB_SERVER_URL=wss://fps.example.com
    if(FPS_WEB_SERVER_URL)
        target_compile_definitions(fps_client PRIVATE FPS_WEB_SERVER_URL="${FPS_WEB_SERVER_URL}")
    endif()
    # Emit fps_client.html (+ .js, .wasm, .data).
    set_target_properties(fps_client PROPERTIES SUFFIX ".html")

    # The favicon is a real HTTP request, so it cannot ride in the --preload-file
    # bundle (that is a virtual filesystem inside the wasm module, invisible to
    # the browser's tab chrome). It has to sit next to the .html as a plain file.
    add_custom_command(TARGET fps_client POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_SOURCE_DIR}/web/favicon.ico"
                "$<TARGET_FILE_DIR:fps_client>/favicon.ico"
        COMMENT "Copying favicon.ico next to the client"
        VERBATIM)
endif()

endif()  # FPS_BUILD_CLIENT

# The dedicated server is native-only (no browser build).
if(NOT EMSCRIPTEN)
    add_executable(fps_server
        server/main.cpp
        server/server_game.cpp
        server/server_game.h
    )
    target_link_libraries(fps_server PRIVATE fps::engine fps::game_shared)
    fps_set_warnings(fps_server)
    fps_enable_sanitizers(fps_server)
endif()
