#include "engine/rendering/mesh_data.h"

#include <cmath>

namespace eng {

MeshData MeshData::unit_cube() {
    MeshData mesh;
    mesh.vertices.reserve(24);
    mesh.indices.reserve(36);

    struct Face {
        glm::vec3 normal;
        glm::vec3 corners[4];  // CCW from outside
    };
    constexpr float h = 0.5f;
    const Face faces[6] = {
        // +X
        {{1, 0, 0}, {{h, -h, h}, {h, -h, -h}, {h, h, -h}, {h, h, h}}},
        // -X
        {{-1, 0, 0}, {{-h, -h, -h}, {-h, -h, h}, {-h, h, h}, {-h, h, -h}}},
        // +Y
        {{0, 1, 0}, {{-h, h, h}, {h, h, h}, {h, h, -h}, {-h, h, -h}}},
        // -Y
        {{0, -1, 0}, {{-h, -h, -h}, {h, -h, -h}, {h, -h, h}, {-h, -h, h}}},
        // +Z
        {{0, 0, 1}, {{-h, -h, h}, {h, -h, h}, {h, h, h}, {-h, h, h}}},
        // -Z
        {{0, 0, -1}, {{h, -h, -h}, {-h, -h, -h}, {-h, h, -h}, {h, h, -h}}},
    };
    const glm::vec2 uvs[4] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};

    for (const Face& face : faces) {
        const auto base = static_cast<std::uint32_t>(mesh.vertices.size());
        for (int i = 0; i < 4; ++i) {
            mesh.vertices.push_back({face.corners[i], face.normal, uvs[i]});
        }
        for (const std::uint32_t offset : {0u, 1u, 2u, 0u, 2u, 3u}) {
            mesh.indices.push_back(base + offset);
        }
    }
    return mesh;
}

void generate_tangents(MeshData& mesh) {
    // Accumulate both basis vectors per vertex. The bitangent sum is only
    // kept to recover the handedness at the end -- a mirrored UV island has
    // a tangent frame of the opposite chirality, and getting that sign wrong
    // flips the normal map's green channel on exactly those triangles.
    std::vector<glm::vec3> tangents(mesh.vertices.size(), glm::vec3{0.0f});
    std::vector<glm::vec3> bitangents(mesh.vertices.size(), glm::vec3{0.0f});

    for (std::size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
        const std::uint32_t i0 = mesh.indices[i + 0];
        const std::uint32_t i1 = mesh.indices[i + 1];
        const std::uint32_t i2 = mesh.indices[i + 2];
        const Vertex& v0 = mesh.vertices[i0];
        const Vertex& v1 = mesh.vertices[i1];
        const Vertex& v2 = mesh.vertices[i2];

        const glm::vec3 edge1 = v1.position - v0.position;
        const glm::vec3 edge2 = v2.position - v0.position;
        const glm::vec2 duv1 = v1.uv - v0.uv;
        const glm::vec2 duv2 = v2.uv - v0.uv;

        // Zero UV area means the triangle carries no information about which
        // way U runs, so it contributes nothing rather than a division by
        // (nearly) zero.
        const float determinant = duv1.x * duv2.y - duv2.x * duv1.y;
        if (std::abs(determinant) < 1e-12f) {
            continue;
        }
        const float inverse = 1.0f / determinant;
        const glm::vec3 tangent = (edge1 * duv2.y - edge2 * duv1.y) * inverse;
        const glm::vec3 bitangent = (edge2 * duv1.x - edge1 * duv2.x) * inverse;
        for (const std::uint32_t index : {i0, i1, i2}) {
            tangents[index] += tangent;
            bitangents[index] += bitangent;
        }
    }

    for (std::size_t v = 0; v < mesh.vertices.size(); ++v) {
        Vertex& vertex = mesh.vertices[v];
        const glm::vec3 normal = vertex.normal;
        // Gram-Schmidt: the averaged tangent is only approximately in the
        // tangent plane once several faces have contributed to it.
        const glm::vec3 projected = tangents[v] - normal * glm::dot(normal, tangents[v]);
        const float length = glm::length(projected);
        if (!(length > 1e-8f)) {
            vertex.tangent = glm::vec4{0.0f};  // no solvable frame; shader falls back
            continue;
        }
        const glm::vec3 unit = projected / length;
        const float handedness =
            glm::dot(glm::cross(normal, unit), bitangents[v]) < 0.0f ? -1.0f : 1.0f;
        vertex.tangent = glm::vec4{unit, handedness};
    }
}

}  // namespace eng
