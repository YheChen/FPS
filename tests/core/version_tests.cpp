#include "engine/core/version.h"

#include <catch2/catch_test_macros.hpp>

#include <string_view>

namespace {

TEST_CASE("engine reports a semantic version", "[version]") {
    const std::string_view version = eng::version_string();

    REQUIRE_FALSE(version.empty());
    // Expect "major.minor.patch" injected from CMake, not the fallback.
    CHECK(version != "unknown");
    CHECK(version.find('.') != std::string_view::npos);
}

}  // namespace
