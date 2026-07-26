#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "engine/rendering/mesh_data.h"

// Minimal glTF 2.0 import (via cgltf), headless-safe: produces CPU data
// only. Supported subset: .glb/.gltf, POSITION/NORMAL/TEXCOORD_0
// attributes, indexed triangles, pbrMetallicRoughness (baseColorFactor,
// baseColorTexture, metallic/roughness factors) and emissive factor.
// Skins, animations and cameras are intentionally ignored until the game
// needs them.
namespace eng {

// A decoded image, always RGBA8. Only produced when image decoding is
// requested: the dedicated server loads the same maps for collision and has
// no use for pixels.
struct GltfImage {
    std::string name;
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> pixels;  // width * height * 4, top-left origin

    bool valid() const { return width > 0 && height > 0 && !pixels.empty(); }
};

struct GltfMaterial {
    std::string name;
    glm::vec4 base_color{1.0f};
    // Index into GltfModel::images, or -1 for an untextured material.
    int base_color_image = -1;
    float metallic = 0.0f;
    float roughness = 1.0f;
    glm::vec3 emissive{0.0f};
};

struct GltfPrimitive {
    MeshData mesh;
    int material = -1;  // index into GltfModel::materials, -1 = none
};

struct GltfMesh {
    std::string name;
    std::vector<GltfPrimitive> primitives;
};

// Nodes are flattened: `transform` is the world transform. Nodes without a
// mesh (mesh == -1) are markers (e.g. "spawn_0"); games interpret them by
// name.
struct GltfNode {
    std::string name;
    glm::mat4 transform{1.0f};
    int mesh = -1;
};

struct GltfModel {
    std::vector<GltfImage> images;
    std::vector<GltfMaterial> materials;
    std::vector<GltfMesh> meshes;
    std::vector<GltfNode> nodes;
};

// Loads a glTF file. Returns nullopt (with error logs) on any parse or
// validation failure. `decode_images` controls whether texture pixels are
// decoded: the client wants them, the headless server does not (it only
// needs collision geometry), and decoding is the expensive part.
std::optional<GltfModel> load_gltf(const std::filesystem::path& path, bool decode_images = true);

}  // namespace eng
