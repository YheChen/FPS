#include "engine/assets/gltf_loader.h"

#define CGLTF_IMPLEMENTATION
#include <cgltf.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO_WRITE
#include <stb_image.h>

#include <cstring>
#include <fstream>
#include <iterator>

#include "engine/core/log.h"

namespace eng {

namespace {

std::string path_string(const std::filesystem::path& path) {
    return path.string();
}

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
            mesh.vertices[v].normal = {scratch[v * 3 + 0], scratch[v * 3 + 1], scratch[v * 3 + 2]};
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
                       static_cast<std::uint64_t>(index), static_cast<std::uint64_t>(vertex_count));
            return std::nullopt;
        }
        mesh.indices[i] = static_cast<std::uint32_t>(index);
    }
    return mesh;
}

// Decodes one glTF image to RGBA8. Images live either inside the file
// (a buffer view, which is what .glb uses) or beside it as a URI.
GltfImage decode_image(const cgltf_image& image, const std::filesystem::path& gltf_path) {
    GltfImage out;
    out.name = image.name != nullptr ? image.name : "";

    const stbi_uc* encoded = nullptr;
    int encoded_size = 0;
    std::vector<std::uint8_t> file_bytes;

    if (image.buffer_view != nullptr && image.buffer_view->buffer != nullptr &&
        image.buffer_view->buffer->data != nullptr) {
        encoded = static_cast<const stbi_uc*>(image.buffer_view->buffer->data) +
                  image.buffer_view->offset;
        encoded_size = static_cast<int>(image.buffer_view->size);
    } else if (image.uri != nullptr) {
        if (std::strncmp(image.uri, "data:", 5) == 0) {
            log::warn("glTF '{}': data: URI images are not supported", path_string(gltf_path));
            return out;
        }
        const std::filesystem::path file = gltf_path.parent_path() / image.uri;
        std::ifstream stream(file, std::ios::binary);
        if (!stream) {
            log::warn("glTF '{}': missing texture file '{}'", path_string(gltf_path), image.uri);
            return out;
        }
        file_bytes.assign(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
        encoded = file_bytes.data();
        encoded_size = static_cast<int>(file_bytes.size());
    } else {
        return out;  // nothing to decode
    }

    int channels = 0;
    stbi_uc* pixels =
        stbi_load_from_memory(encoded, encoded_size, &out.width, &out.height, &channels, 4);
    if (pixels == nullptr) {
        log::warn("glTF '{}': failed to decode image '{}' ({})", path_string(gltf_path), out.name,
                  stbi_failure_reason());
        out.width = 0;
        out.height = 0;
        return out;
    }
    const std::size_t byte_count =
        static_cast<std::size_t>(out.width) * static_cast<std::size_t>(out.height) * 4;
    out.pixels.assign(pixels, pixels + byte_count);
    stbi_image_free(pixels);
    return out;
}

}  // namespace

std::optional<GltfModel> load_gltf(const std::filesystem::path& path, bool decode_images) {
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

    if (decode_images) {
        model.images.reserve(data->images_count);
        for (cgltf_size i = 0; i < data->images_count; ++i) {
            model.images.push_back(decode_image(data->images[i], path));
        }
    } else {
        // Keep indices valid for code that inspects materials without pixels.
        model.images.resize(data->images_count);
    }

    model.materials.reserve(data->materials_count);
    for (cgltf_size i = 0; i < data->materials_count; ++i) {
        const cgltf_material& material = data->materials[i];
        GltfMaterial out;
        out.name = material.name != nullptr ? material.name : "";
        if (material.has_pbr_metallic_roughness != 0) {
            const cgltf_pbr_metallic_roughness& pbr = material.pbr_metallic_roughness;
            const float* c = pbr.base_color_factor;
            out.base_color = {c[0], c[1], c[2], c[3]};
            out.metallic = pbr.metallic_factor;
            out.roughness = pbr.roughness_factor;
            if (pbr.base_color_texture.texture != nullptr &&
                pbr.base_color_texture.texture->image != nullptr) {
                out.base_color_image =
                    static_cast<int>(pbr.base_color_texture.texture->image - data->images);
            }
        }
        out.emissive = {material.emissive_factor[0], material.emissive_factor[1],
                        material.emissive_factor[2]};
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

    std::size_t decoded = 0;
    for (const GltfImage& image : model.images) {
        decoded += image.valid() ? 1 : 0;
    }
    log::info("glTF '{}': {} nodes, {} meshes, {} materials, {}/{} images decoded", path.string(),
              model.nodes.size(), model.meshes.size(), model.materials.size(), decoded,
              model.images.size());
    return model;
}

}  // namespace eng
