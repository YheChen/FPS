#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "engine/rendering/mesh_data.h"

// Minimal glTF 2.0 import (via cgltf), headless-safe: produces CPU data
// only. Supported subset: .glb/.gltf, POSITION/NORMAL/TEXCOORD_0
// attributes, indexed triangles, pbrMetallicRoughness baseColorFactor.
// Textures/images, skins, animations and cameras are intentionally ignored
// until the game needs them.
namespace eng {

struct GltfMaterial {
    std::string name;
    glm::vec4 base_color{1.0f};
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
    std::vector<GltfMaterial> materials;
    std::vector<GltfMesh> meshes;
    std::vector<GltfNode> nodes;
};

// Loads a glTF file. Returns nullopt (with error logs) on any parse or
// validation failure.
std::optional<GltfModel> load_gltf(const std::filesystem::path& path);

}  // namespace eng
