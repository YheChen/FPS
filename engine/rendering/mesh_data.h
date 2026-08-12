#pragma once

#include <cstdint>
#include <vector>

#include <glm/glm.hpp>

// CPU-side mesh representation. Lives in the headless-safe engine target:
// the server uses it for collision geometry; the client uploads it to the
// GPU via GpuMesh (engine_platform).
namespace eng {

// One vertex format for everything, skinned or not, normal-mapped or not.
// The skinning and tangent attributes cost 48 bytes on static geometry that
// never uses them, which is worth it to avoid a second vertex layout, a
// second GpuMesh path and a second shader family: the arena is a few hundred
// vertices, so the memory is noise.
//
// An unskinned vertex has all weights at 0, which the shader treats as "use
// the model matrix unchanged". A vertex with no meaningful tangent frame has
// a zero tangent, which the shader treats as "use the interpolated vertex
// normal" -- normalizing a zero vector would hand the lighting a NaN.
struct Vertex {
    glm::vec3 position{0.0f};
    glm::vec3 normal{0.0f, 1.0f, 0.0f};
    glm::vec2 uv{0.0f};
    // xyz is the surface direction of +U; w is the bitangent's handedness
    // (+/-1), which is how glTF stores it and how mirrored UVs stay correct.
    glm::vec4 tangent{0.0f};
    glm::uvec4 joints{0u};    // indices into the skin's joint list
    glm::vec4 weights{0.0f};  // parallel to `joints`, sums to 1 when skinned

    bool skinned() const { return weights.x + weights.y + weights.z + weights.w > 0.0f; }
};

struct MeshData {
    std::vector<Vertex> vertices;
    std::vector<std::uint32_t> indices;

    // Axis-aligned unit cube centered on the origin (1x1x1), 24 vertices
    // (per-face normals), 36 indices, CCW winding when viewed from outside.
    //
    // Tangents are left at zero: this is the stand-in mesh for untextured
    // geometry, so a tangent frame would be arithmetic nobody samples, and
    // leaving it zero keeps the shader's fallback path exercised every frame
    // rather than only by whatever mesh happens to lack UVs.
    static MeshData unit_cube();
};

// Fills in per-vertex tangents from positions and UVs (Lengyel's method:
// accumulate each triangle's UV-space gradient onto its vertices, then
// orthonormalize against the vertex normal).
//
// Vertices the mesh gives no way to solve for -- degenerate UVs, a tangent
// that came out parallel to the normal, or a mesh with no UVs at all -- keep
// a zero tangent rather than an invented one, because a wrong tangent frame
// lights a surface confidently in the wrong direction while a zero one falls
// back to the vertex normal.
void generate_tangents(MeshData& mesh);

}  // namespace eng
