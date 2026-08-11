#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include "engine/rendering/mesh_data.h"

// Minimal glTF 2.0 import (via cgltf), headless-safe: produces CPU data
// only. Supported subset: .glb/.gltf, POSITION/NORMAL/TEXCOORD_0
// attributes, indexed triangles, pbrMetallicRoughness (baseColorFactor,
// baseColorTexture, metallic/roughness factors), normalTexture and emissive
// factor. Cameras are intentionally ignored until the game needs them.
//
// Tangents are always derived from positions and UVs rather than read from
// a TANGENT accessor: nothing this project generates authors one, and one
// code path cannot disagree with itself about handedness.
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
    // Tangent-space normal map, also an index into GltfModel::images, -1 for
    // a material that just uses its geometric normal. Unlike the base color
    // this image is NOT sRGB-encoded: it holds directions, not colours.
    int normal_image = -1;
    float normal_scale = 1.0f;  // glTF normalTexture.scale, multiplies XY
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

// `transform` is the flattened world transform of the rest pose, which is
// what static geometry (arena collision, scene entities) wants. The local
// TRS and hierarchy are also kept, because animation has to re-pose the
// hierarchy every frame and cannot work from a flattened matrix.
//
// Nodes without a mesh (mesh == -1) are markers (e.g. "spawn_0") or joints;
// games interpret them by name.
struct GltfNode {
    std::string name;
    glm::mat4 transform{1.0f};  // world, rest pose
    glm::vec3 translation{0.0f};
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};  // w, x, y, z
    glm::vec3 scale{1.0f};
    int mesh = -1;
    int skin = -1;    // index into GltfModel::skins, -1 = not skinned
    int parent = -1;  // index into GltfModel::nodes, -1 = a scene root
    std::vector<int> children;
};

// A skin binds a mesh's vertex joint indices to a set of nodes.
struct GltfSkin {
    std::string name;
    // Vertex JOINTS_0 values index into THIS list, not into `nodes`.
    std::vector<int> joints;
    // Parallel to `joints`: transforms model space into each joint's local
    // space, undoing the rest pose.
    std::vector<glm::mat4> inverse_bind_matrices;
    int skeleton_root = -1;  // node index, -1 if the file did not say
};

enum class GltfAnimationPath : std::uint8_t { Translation, Rotation, Scale };

// One animated property of one node. `times` and `values` are parallel.
// Rotations are quaternions; translation and scale use only xyz.
struct GltfAnimationChannel {
    int node = -1;
    GltfAnimationPath path = GltfAnimationPath::Rotation;
    std::vector<float> times;
    std::vector<glm::vec4> values;
};

struct GltfAnimation {
    std::string name;
    float duration_seconds = 0.0f;
    std::vector<GltfAnimationChannel> channels;
};

struct GltfModel {
    std::vector<GltfImage> images;
    std::vector<GltfMaterial> materials;
    std::vector<GltfMesh> meshes;
    std::vector<GltfNode> nodes;
    std::vector<GltfSkin> skins;
    std::vector<GltfAnimation> animations;
};

// Loads a glTF file. Returns nullopt (with error logs) on any parse or
// validation failure. `decode_images` controls whether texture pixels are
// decoded: the client wants them, the headless server does not (it only
// needs collision geometry), and decoding is the expensive part.
std::optional<GltfModel> load_gltf(const std::filesystem::path& path, bool decode_images = true);

}  // namespace eng
