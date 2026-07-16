add_executable(fps_client
    client/main.cpp
)
target_link_libraries(fps_client PRIVATE fps::engine)
fps_set_warnings(fps_client)
fps_enable_sanitizers(fps_client)

add_executable(fps_server
    server/main.cpp
)
target_link_libraries(fps_server PRIVATE fps::engine)
fps_set_warnings(fps_server)
fps_enable_sanitizers(fps_server)
