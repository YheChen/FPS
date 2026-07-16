#include "engine/rendering/mesh_data.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

namespace {

TEST_CASE("unit cube has per-face vertices and full index coverage", "[mesh]") {
    const eng::MeshData cube = eng::MeshData::unit_cube();
    CHECK(cube.vertices.size() == 24);  // 6 faces * 4 corners
    CHECK(cube.indices.size() == 36);   // 6 faces * 2 triangles * 3

    for (const std::uint32_t index : cube.indices) {
        CHECK(index < cube.vertices.size());
    }
}

TEST_CASE("unit cube is centered with half-extent 0.5 and unit normals", "[mesh]") {
    const eng::MeshData cube = eng::MeshData::unit_cube();
    for (const eng::Vertex& v : cube.vertices) {
        CHECK(std::abs(v.position.x) == Catch::Approx(0.5f));
        CHECK(std::abs(v.position.y) == Catch::Approx(0.5f));
        CHECK(std::abs(v.position.z) == Catch::Approx(0.5f));
        CHECK(glm::length(v.normal) == Catch::Approx(1.0f));
        // Normal must point the same way as the face the vertex is on.
        CHECK(glm::dot(v.normal, v.position) > 0.0f);
    }
}

}  // namespace
