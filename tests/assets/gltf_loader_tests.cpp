#include "engine/assets/gltf_loader.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/assets/asset_cache.h"
#include "engine/assets/paths.h"

namespace {

const eng::GltfModel* load_arena() {
    static eng::AssetCache cache{*eng::find_assets_root()};
    return cache.model("maps/arena01.glb");
}

TEST_CASE("arena01.glb loads with expected structure", "[gltf]") {
    const eng::GltfModel* model = load_arena();
    REQUIRE(model != nullptr);

    CHECK(model->materials.size() == 4);
    CHECK(model->meshes.size() == 4);
    CHECK(model->nodes.size() == 22);

    // Every mesh is an indexed cube: 24 vertices, 36 indices, valid indices.
    for (const eng::GltfMesh& mesh : model->meshes) {
        REQUIRE(mesh.primitives.size() == 1);
        const eng::MeshData& data = mesh.primitives[0].mesh;
        CHECK(data.vertices.size() == 24);
        CHECK(data.indices.size() == 36);
        for (const std::uint32_t index : data.indices) {
            CHECK(index < data.vertices.size());
        }
        CHECK(mesh.primitives[0].material >= 0);
    }
}

TEST_CASE("arena node transforms and markers are imported", "[gltf]") {
    const eng::GltfModel* model = load_arena();
    REQUIRE(model != nullptr);

    const eng::GltfNode* floor = nullptr;
    int spawn_count = 0;
    for (const eng::GltfNode& node : model->nodes) {
        if (node.name == "floor") {
            floor = &node;
        }
        if (node.name.starts_with("spawn_")) {
            CHECK(node.mesh == -1);  // markers have no geometry
            ++spawn_count;
        }
    }
    REQUIRE(floor != nullptr);
    CHECK(spawn_count == 8);

    // floor: translation (0,-0.5,0), scale (40,1,40).
    CHECK(floor->transform[3][1] == Catch::Approx(-0.5f));
    CHECK(floor->transform[0][0] == Catch::Approx(40.0f));
    CHECK(floor->transform[2][2] == Catch::Approx(40.0f));
    CHECK(floor->mesh >= 0);
}

TEST_CASE("asset cache returns the same pointer and rejects escapes", "[assets]") {
    eng::AssetCache cache{*eng::find_assets_root()};
    const eng::GltfModel* a = cache.model("maps/arena01.glb");
    const eng::GltfModel* b = cache.model("./maps//arena01.glb");  // same after normalize
    REQUIRE(a != nullptr);
    CHECK(a == b);

    CHECK(cache.model("../CMakeLists.txt") == nullptr);
    CHECK(cache.model("maps/does_not_exist.glb") == nullptr);
}

}  // namespace
