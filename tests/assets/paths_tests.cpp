#include "engine/assets/paths.h"

#include <catch2/catch_test_macros.hpp>

namespace {

TEST_CASE("normalize_asset_path canonicalizes separators and dot segments", "[assets]") {
    CHECK(eng::normalize_asset_path("maps/arena01.glb") == "maps/arena01.glb");
    CHECK(eng::normalize_asset_path("maps\\arena01.glb") == "maps/arena01.glb");
    CHECK(eng::normalize_asset_path("./maps/./arena01.glb") == "maps/arena01.glb");
    CHECK(eng::normalize_asset_path("/maps//arena01.glb") == "maps/arena01.glb");
    CHECK(eng::normalize_asset_path("") == "");
}

TEST_CASE("asset_path_escapes_root detects parent traversal", "[assets]") {
    CHECK(eng::asset_path_escapes_root("../secrets.txt"));
    CHECK(eng::asset_path_escapes_root("maps/../../etc/passwd"));
    CHECK_FALSE(eng::asset_path_escapes_root("maps/arena01.glb"));
    CHECK_FALSE(eng::asset_path_escapes_root("maps/..hidden/file"));
}

TEST_CASE("find_assets_root walks up from the build tree", "[assets]") {
    // Tests run somewhere inside the repo (build/debug/tests); the repo
    // root has assets/.
    const auto root = eng::find_assets_root();
    REQUIRE(root.has_value());
    CHECK(root->filename() == "assets");
    CHECK(std::filesystem::is_directory(*root));
}

}  // namespace
