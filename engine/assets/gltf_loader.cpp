#include "engine/assets/gltf_loader.h"

#define CGLTF_IMPLEMENTATION
#include <cgltf.h>

#include <cstring>

#include "engine/core/log.h"

namespace eng {

namespace {

std::optional<MeshData> read_primitive(const cgltf_primitive& primitive,
                                       const std::filesystem::path& path) {
    if (primitive.type != cgltf_primitive_type_triangles) {
        log::warn("glTF '{}': skipping non-triangle primitive", path.string());
        return std::nullopt;
    }

    const cgltf_accessor* positions = nullptr;
    const cgltf_accessor* normals = nullptr;
    const cgltf_accessor* uvs = nullptr;
    for (cgltf_size i = 0; i < primitive.attributes_count; ++i) {
        const cgltf_attribute& attribute = primitive.attributes[i];
        if (attribute.type == cgltf_attribute_type_position) {
            positions = attribute.data;
        } else if (attribute.type == cgltf_attribute_type_normal) {
            normals = attribute.data;
        } else if (attribute.type == cgltf_attribute_type_texcoord && attribute.index == 0) {
            uvs = attribute.data;
        }
    }
    if (positions == nullptr) {
        log::error("glTF '{}': primitive has no POSITION attribute", path.string());
        return std::nullopt;
    }
    if (primitive.indices == nullptr) {
        log::error("glTF '{}': non-indexed primitives are not supported", path.string());
        return std::nullopt;
    }

    const cgltf_size vertex_count = positions->count;
    MeshData mesh;
    mesh.vertices.resize(vertex_count);

    std::vector<float> scratch(vertex_count * 3);
    if (cgltf_accessor_unpack_floats(positions, scratch.data(), vertex_count * 3) == 0) {
        log::error("glTF '{}': failed to unpack POSITION", path.string());
        return std::nullopt;
    }
    for (cgltf_size v = 0; v < vertex_count; ++v) {
        mesh.vertices[v].position = {scratch[v * 3 + 0], scratch[v * 3 + 1], scratch[v * 3 + 2]};
    }

    if (normals != nullptr && normals->count == vertex_count &&
        cgltf_accessor_unpack_floats(normals, scratch.data(), vertex_count * 3) != 0) {
        for (cgltf_size v = 0; v < vertex_count; ++v) {
            mesh.vertices[v].normal = {scratch[v * 3 + 0], scratch[v * 3 + 1],
                                       scratch[v * 3 + 2]};
        }
    }

    if (uvs != nullptr && uvs->count == vertex_count) {
        std::vector<float> uv_scratch(vertex_count * 2);
        if (cgltf_accessor_unpack_floats(uvs, uv_scratch.data(), vertex_count * 2) != 0) {
            for (cgltf_size v = 0; v < vertex_count; ++v) {
                mesh.vertices[v].uv = {uv_scratch[v * 2 + 0], uv_scratch[v * 2 + 1]};
            }
        }
    }

    mesh.indices.resize(primitive.indices->count);
    for (cgltf_size i = 0; i < primitive.indices->count; ++i) {
        const cgltf_size index = cgltf_accessor_read_index(primitive.indices, i);
        if (index >= vertex_count) {
            log::error("glTF '{}': index {} out of range ({} vertices)", path.string(),
                       static_cast<std::uint64_t>(index),
                       static_cast<std::uint64_t>(vertex_count));
            return std::nullopt;
        }
        mesh.indices[i] = static_cast<std::uint32_t>(index);
    }
    return mesh;
}

}  // namespace

std::optional<GltfModel> load_gltf(const std::filesystem::path& path) {
    cgltf_options options{};
    cgltf_data* data = nullptr;
    cgltf_result result = cgltf_parse_file(&options, path.string().c_str(), &data);
    if (result != cgltf_result_success) {
        log::error("glTF '{}': parse failed (cgltf result {})", path.string(),
                   static_cast<int>(result));
        return std::nullopt;
    }
    // RAII for the cgltf allocation.
    struct DataGuard {
        cgltf_data* data;
        ~DataGuard() { cgltf_free(data); }
    } guard{data};

    result = cgltf_load_buffers(&options, data, path.string().c_str());
    if (result != cgltf_result_success) {
        log::error("glTF '{}': buffer load failed (cgltf result {})", path.string(),
                   static_cast<int>(result));
        return std::nullopt;
    }
    if (cgltf_validate(data) != cgltf_result_success) {
        log::error("glTF '{}': validation failed", path.string());
        return std::nullopt;
    }

    GltfModel model;

    model.materials.reserve(data->materials_count);
    for (cgltf_size i = 0; i < data->materials_count; ++i) {
        const cgltf_material& material = data->materials[i];
        GltfMaterial out;
        out.name = material.name != nullptr ? material.name : "";
        if (material.has_pbr_metallic_roughness != 0) {
            const float* c = material.pbr_metallic_roughness.base_color_factor;
            out.base_color = {c[0], c[1], c[2], c[3]};
        }
        model.materials.push_back(std::move(out));
    }

    model.meshes.reserve(data->meshes_count);
    for (cgltf_size i = 0; i < data->meshes_count; ++i) {
        const cgltf_mesh& mesh = data->meshes[i];
        GltfMesh out;
        out.name = mesh.name != nullptr ? mesh.name : "";
        for (cgltf_size p = 0; p < mesh.primitives_count; ++p) {
            auto primitive_mesh = read_primitive(mesh.primitives[p], path);
            if (!primitive_mesh) {
                return std::nullopt;
            }
            GltfPrimitive primitive;
            primitive.mesh = std::move(*primitive_mesh);
            if (mesh.primitives[p].material != nullptr) {
                primitive.material =
                    static_cast<int>(mesh.primitives[p].material - data->materials);
            }
            out.primitives.push_back(std::move(primitive));
        }
        model.meshes.push_back(std::move(out));
    }

    for (cgltf_size i = 0; i < data->nodes_count; ++i) {
        const cgltf_node& node = data->nodes[i];
        GltfNode out;
        out.name = node.name != nullptr ? node.name : "";
        cgltf_node_transform_world(&node, &out.transform[0][0]);
        if (node.mesh != nullptr) {
            out.mesh = static_cast<int>(node.mesh - data->meshes);
        }
        model.nodes.push_back(std::move(out));
    }

    log::info("glTF '{}': {} nodes, {} meshes, {} materials", path.string(), model.nodes.size(),
              model.meshes.size(), model.materials.size());
    return model;
}

}  // namespace eng
