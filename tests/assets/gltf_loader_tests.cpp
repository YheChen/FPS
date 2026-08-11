#include "engine/assets/gltf_loader.h"

#include <algorithm>

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

    // One mesh per box (so UVs can be scaled to each box's world size),
    // plus 8 spawn markers among the nodes.
    CHECK(model->materials.size() == 4);
    CHECK(model->meshes.size() == 14);
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

// Counting nodes proves a file parsed. These are the properties that decide
// whether a map can actually be HOSTED, and they are checked for every map
// rather than for one -- a second map is only worth having if adding a third
// is a data change, and that is only true if the invariants are enforced
// somewhere other than in whoever wrote the layout's head.
TEST_CASE("every shipped map satisfies what the server relies on", "[gltf]") {
    static eng::AssetCache cache{*eng::find_assets_root()};

    for (const char* name : {"maps/arena01.glb", "maps/arena02.glb"}) {
        CAPTURE(name);
        const eng::GltfModel* model = cache.model(name);
        REQUIRE(model != nullptr);

        std::vector<glm::vec3> spawns;
        std::size_t collision_primitives = 0;
        glm::vec3 lo{1e9f};
        glm::vec3 hi{-1e9f};
        for (const eng::GltfNode& node : model->nodes) {
            if (node.name.starts_with("spawn_")) {
                spawns.emplace_back(node.transform[3]);
            }
            if (node.mesh < 0) {
                continue;
            }
            for (const eng::GltfPrimitive& primitive :
                 model->meshes[static_cast<std::size_t>(node.mesh)].primitives) {
                ++collision_primitives;
                for (const eng::Vertex& vertex : primitive.mesh.vertices) {
                    const glm::vec3 world =
                        glm::vec3(node.transform * glm::vec4(vertex.position, 1.0f));
                    lo = glm::min(lo, world);
                    hi = glm::max(hi, world);
                }
            }
        }

        // The server picks spawn positions purely by node name. With none,
        // every player materializes at the origin -- inside whatever is there.
        CHECK(spawns.size() >= 4);
        // Geometry is what the server turns into collision; a map with none
        // is a void players fall through forever.
        CHECK(collision_primitives > 0);

        for (const glm::vec3& spawn : spawns) {
            CAPTURE(spawn.x, spawn.y, spawn.z);
            // Floor top is y = 0 by convention, so a spawn at or below it
            // starts the player embedded in the floor.
            CHECK(spawn.y > 0.0f);
            // Strictly inside the geometry's extent in the ground plane. A
            // spawn outside the walls is a player who falls out of the world
            // on the first tick, and there is no kill volume to catch them.
            CHECK(spawn.x > lo.x);
            CHECK(spawn.x < hi.x);
            CHECK(spawn.z > lo.z);
            CHECK(spawn.z < hi.z);
        }
    }
}

TEST_CASE("arena materials reference decoded textures", "[gltf]") {
    const eng::GltfModel* model = load_arena();
    REQUIRE(model != nullptr);

    // An albedo and a normal map per material.
    REQUIRE(model->images.size() == 8);
    for (const eng::GltfImage& image : model->images) {
        CHECK(image.valid());
        CHECK(image.width == image.height);
        // RGBA8, tightly packed.
        CHECK(image.pixels.size() ==
              static_cast<std::size_t>(image.width) * static_cast<std::size_t>(image.height) * 4);
    }

    for (const eng::GltfMaterial& material : model->materials) {
        CAPTURE(material.name);
        REQUIRE(material.base_color_image >= 0);
        CHECK(static_cast<std::size_t>(material.base_color_image) < model->images.size());
        CHECK(material.roughness > 0.0f);

        // Every arena material is normal-mapped, and never by the same image
        // it uses for albedo: the two are uploaded with different sRGB
        // settings, so sharing one would have to be wrong for one of them.
        REQUIRE(material.normal_image >= 0);
        CHECK(static_cast<std::size_t>(material.normal_image) < model->images.size());
        CHECK(material.normal_image != material.base_color_image);
        CHECK(material.normal_scale == Catch::Approx(1.0f));

        // Normal maps ship at half the albedo's resolution to keep the
        // browser download down; if that ratio ever changes, the size budget
        // in tools/gen_textures.py changed with it.
        const eng::GltfImage& albedo =
            model->images[static_cast<std::size_t>(material.base_color_image)];
        const eng::GltfImage& normal =
            model->images[static_cast<std::size_t>(material.normal_image)];
        CHECK(albedo.width == 512);
        CHECK(normal.width == 256);
    }
}

TEST_CASE("arena geometry carries a usable tangent frame", "[gltf]") {
    const eng::GltfModel* model = load_arena();
    REQUIRE(model != nullptr);

    for (const eng::GltfMesh& mesh : model->meshes) {
        CAPTURE(mesh.name);
        for (const eng::GltfPrimitive& primitive : mesh.primitives) {
            for (const eng::Vertex& vertex : primitive.mesh.vertices) {
                // Unit length, perpendicular to the normal, and a handedness
                // of exactly +/-1: anything else and the shader's TBN is not
                // orthonormal, which shears the normal map.
                CHECK(glm::length(glm::vec3(vertex.tangent)) == Catch::Approx(1.0f));
                CHECK(glm::dot(glm::vec3(vertex.tangent), vertex.normal) ==
                      Catch::Approx(0.0f).margin(1e-5));
                CHECK(std::abs(vertex.tangent.w) == Catch::Approx(1.0f));
            }
        }
    }
}

TEST_CASE("arena UVs are scaled to world size so textures tile", "[gltf]") {
    const eng::GltfModel* model = load_arena();
    REQUIRE(model != nullptr);

    const eng::GltfNode* floor = nullptr;
    for (const eng::GltfNode& node : model->nodes) {
        if (node.name == "floor") {
            floor = &node;
        }
    }
    REQUIRE(floor != nullptr);
    REQUIRE(floor->mesh >= 0);

    // The floor is 40 m across at 0.5 texels/m, so its UVs must run well past
    // 1.0 — a plain 0..1 unwrap would stretch one texture over the whole map.
    const eng::MeshData& data =
        model->meshes[static_cast<std::size_t>(floor->mesh)].primitives[0].mesh;
    float max_u = 0.0f;
    for (const eng::Vertex& vertex : data.vertices) {
        max_u = std::max(max_u, vertex.uv.x);
    }
    CHECK(max_u == Catch::Approx(20.0f));
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

TEST_CASE("image decoding can be skipped for headless loads", "[gltf]") {
    // This is how the dedicated server loads maps: it needs collision
    // geometry, never pixels.
    eng::AssetCache cache{*eng::find_assets_root(), /*decode_images=*/false};
    const eng::GltfModel* model = cache.model("maps/arena01.glb");
    REQUIRE(model != nullptr);

    // The slots survive so material image indices stay meaningful, but
    // nothing was decoded into them.
    REQUIRE(model->images.size() == 8);
    for (const eng::GltfImage& image : model->images) {
        CHECK_FALSE(image.valid());
        CHECK(image.pixels.empty());
    }
    CHECK(model->meshes.size() == 14);
    CHECK(model->materials.size() == 4);
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
