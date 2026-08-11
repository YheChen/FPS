#include "engine/rendering/mesh_data.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

namespace {

// A quad in the XY plane facing +Z, with the caller's UVs on its corners.
eng::MeshData uv_quad(const glm::vec2 (&uvs)[4]) {
    eng::MeshData mesh;
    const glm::vec3 corners[4] = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}};
    for (int i = 0; i < 4; ++i) {
        eng::Vertex vertex;
        vertex.position = corners[i];
        vertex.normal = {0, 0, 1};
        vertex.uv = uvs[i];
        mesh.vertices.push_back(vertex);
    }
    mesh.indices = {0, 1, 2, 0, 2, 3};
    return mesh;
}

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

TEST_CASE("unit cube ships zero tangents for the shader to fall back on", "[mesh]") {
    // This is the untextured stand-in mesh, and it is drawn every frame, so
    // it is also what keeps the lit shader's no-tangent-frame branch honest.
    const eng::MeshData cube = eng::MeshData::unit_cube();
    for (const eng::Vertex& v : cube.vertices) {
        CHECK(v.tangent == glm::vec4{0.0f});
    }
}

TEST_CASE("tangents follow the U axis and record handedness", "[mesh]") {
    SECTION("U runs along +X") {
        const glm::vec2 uvs[4] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
        eng::MeshData quad = uv_quad(uvs);
        eng::generate_tangents(quad);
        for (const eng::Vertex& v : quad.vertices) {
            CHECK(v.tangent.x == Catch::Approx(1.0f));
            CHECK(v.tangent.y == Catch::Approx(0.0f).margin(1e-6));
            CHECK(v.tangent.z == Catch::Approx(0.0f).margin(1e-6));
            CHECK(v.tangent.w == Catch::Approx(1.0f));
        }
    }

    SECTION("mirrored U flips the tangent and the handedness") {
        // A mirrored UV island has a left-handed frame. Recording that in w
        // is the whole reason the tangent is a vec4: with w pinned to +1 the
        // bitangent points the wrong way and the normal map's green channel
        // inverts on exactly these triangles.
        const glm::vec2 uvs[4] = {{1, 0}, {0, 0}, {0, 1}, {1, 1}};
        eng::MeshData quad = uv_quad(uvs);
        eng::generate_tangents(quad);
        for (const eng::Vertex& v : quad.vertices) {
            CHECK(v.tangent.x == Catch::Approx(-1.0f));
            CHECK(v.tangent.w == Catch::Approx(-1.0f));
        }
    }
}

TEST_CASE("tangents stay zero where the mesh cannot supply one", "[mesh]") {
    // Every vertex on the same UV point: the triangles have no UV area, so
    // there is no direction "+U" could mean. Inventing one would light the
    // surface confidently wrong; zero routes it to the vertex normal.
    const glm::vec2 uvs[4] = {{0, 0}, {0, 0}, {0, 0}, {0, 0}};
    eng::MeshData quad = uv_quad(uvs);
    eng::generate_tangents(quad);
    for (const eng::Vertex& v : quad.vertices) {
        CHECK(v.tangent == glm::vec4{0.0f});
    }
}

}  // namespace
