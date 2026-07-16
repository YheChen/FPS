include(FetchContent)

FetchContent_Declare(
    Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG v3.8.0
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(Catch2)

add_executable(engine_tests
    core/log_tests.cpp
    core/version_tests.cpp
)
target_link_libraries(engine_tests PRIVATE fps::engine Catch2::Catch2WithMain)
fps_set_warnings(engine_tests)
fps_enable_sanitizers(engine_tests)

list(APPEND CMAKE_MODULE_PATH "${catch2_SOURCE_DIR}/extras")
include(Catch)
catch_discover_tests(engine_tests)
