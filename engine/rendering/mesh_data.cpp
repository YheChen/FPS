#include "engine/rendering/mesh_data.h"

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

}  // namespace eng
