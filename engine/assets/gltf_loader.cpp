#include "engine/assets/gltf_loader.h"

#define CGLTF_IMPLEMENTATION
#include <cgltf.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO_WRITE
#include <stb_image.h>

#include <algorithm>
#include <array>
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
    const cgltf_accessor* joints = nullptr;
    const cgltf_accessor* weights = nullptr;
    for (cgltf_size i = 0; i < primitive.attributes_count; ++i) {
        const cgltf_attribute& attribute = primitive.attributes[i];
        if (attribute.type == cgltf_attribute_type_position) {
            positions = attribute.data;
        } else if (attribute.type == cgltf_attribute_type_normal) {
            normals = attribute.data;
        } else if (attribute.type == cgltf_attribute_type_texcoord && attribute.index == 0) {
            uvs = attribute.data;
        } else if (attribute.type == cgltf_attribute_type_joints && attribute.index == 0) {
            joints = attribute.data;
        } else if (attribute.type == cgltf_attribute_type_weights && attribute.index == 0) {
            weights = attribute.data;
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

    if (joints != nullptr && weights != nullptr && joints->count == vertex_count &&
        weights->count == vertex_count) {
        // Joint indices are integers (u8 or u16 in practice), so they go
        // through read_uint rather than the float unpacker.
        std::array<cgltf_uint, 4> joint_scratch{};
        std::vector<float> weight_scratch(vertex_count * 4);
        const bool weights_ok =
            cgltf_accessor_unpack_floats(weights, weight_scratch.data(), vertex_count * 4) != 0;
        for (cgltf_size v = 0; v < vertex_count; ++v) {
            if (cgltf_accessor_read_uint(joints, v, joint_scratch.data(), 4)) {
                mesh.vertices[v].joints = {joint_scratch[0], joint_scratch[1], joint_scratch[2],
                                           joint_scratch[3]};
            }
            if (weights_ok) {
                mesh.vertices[v].weights = {weight_scratch[v * 4 + 0], weight_scratch[v * 4 + 1],
                                            weight_scratch[v * 4 + 2], weight_scratch[v * 4 + 3]};
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

        // Local TRS as well as the flattened world matrix: animation has to
        // re-pose the hierarchy each frame, which a flattened matrix cannot
        // express. cgltf leaves these at their defaults when the file used a
        // matrix instead, and has_* says which form was authored.
        if (node.has_translation != 0) {
            out.translation = {node.translation[0], node.translation[1], node.translation[2]};
        }
        if (node.has_rotation != 0) {
            // glTF stores xyzw; glm::quat's constructor takes w first.
            out.rotation =
                glm::quat{node.rotation[3], node.rotation[0], node.rotation[1], node.rotation[2]};
        }
        if (node.has_scale != 0) {
            out.scale = {node.scale[0], node.scale[1], node.scale[2]};
        }

        if (node.mesh != nullptr) {
            out.mesh = static_cast<int>(node.mesh - data->meshes);
        }
        if (node.skin != nullptr) {
            out.skin = static_cast<int>(node.skin - data->skins);
        }
        if (node.parent != nullptr) {
            out.parent = static_cast<int>(node.parent - data->nodes);
        }
        out.children.reserve(node.children_count);
        for (cgltf_size c = 0; c < node.children_count; ++c) {
            out.children.push_back(static_cast<int>(node.children[c] - data->nodes));
        }
        model.nodes.push_back(std::move(out));
    }

    model.skins.reserve(data->skins_count);
    for (cgltf_size i = 0; i < data->skins_count; ++i) {
        const cgltf_skin& skin = data->skins[i];
        GltfSkin out;
        out.name = skin.name != nullptr ? skin.name : "";
        out.joints.reserve(skin.joints_count);
        for (cgltf_size j = 0; j < skin.joints_count; ++j) {
            out.joints.push_back(static_cast<int>(skin.joints[j] - data->nodes));
        }
        if (skin.skeleton != nullptr) {
            out.skeleton_root = static_cast<int>(skin.skeleton - data->nodes);
        }
        if (skin.inverse_bind_matrices != nullptr) {
            const cgltf_size count = skin.inverse_bind_matrices->count;
            std::vector<float> scratch(count * 16);
            if (cgltf_accessor_unpack_floats(skin.inverse_bind_matrices, scratch.data(),
                                             count * 16) != 0) {
                out.inverse_bind_matrices.resize(count);
                for (cgltf_size m = 0; m < count; ++m) {
                    // glTF matrices are column-major, same as glm.
                    std::memcpy(&out.inverse_bind_matrices[m][0][0], &scratch[m * 16],
                                sizeof(float) * 16);
                }
            }
        }
        if (out.inverse_bind_matrices.size() != out.joints.size()) {
            // Without a matching inverse bind matrix per joint the skin
            // cannot be posed, and guessing identity would silently render a
            // mangled mesh.
            log::error("glTF '{}': skin '{}' has {} joints but {} inverse bind matrices",
                       path.string(), out.name, out.joints.size(),
                       out.inverse_bind_matrices.size());
            return std::nullopt;
        }
        model.skins.push_back(std::move(out));
    }

    model.animations.reserve(data->animations_count);
    for (cgltf_size i = 0; i < data->animations_count; ++i) {
        const cgltf_animation& animation = data->animations[i];
        GltfAnimation out;
        out.name = animation.name != nullptr ? animation.name : "";
        for (cgltf_size c = 0; c < animation.channels_count; ++c) {
            const cgltf_animation_channel& channel = animation.channels[c];
            if (channel.target_node == nullptr || channel.sampler == nullptr) {
                continue;
            }
            GltfAnimationChannel out_channel;
            out_channel.node = static_cast<int>(channel.target_node - data->nodes);
            switch (channel.target_path) {
                case cgltf_animation_path_type_translation:
                    out_channel.path = GltfAnimationPath::Translation;
                    break;
                case cgltf_animation_path_type_rotation:
                    out_channel.path = GltfAnimationPath::Rotation;
                    break;
                case cgltf_animation_path_type_scale:
                    out_channel.path = GltfAnimationPath::Scale;
                    break;
                default:
                    continue;  // weights (morph targets) are not supported
            }

            const cgltf_accessor* times = channel.sampler->input;
            const cgltf_accessor* values = channel.sampler->output;
            if (times == nullptr || values == nullptr || times->count == 0 ||
                values->count != times->count) {
                continue;
            }
            const cgltf_size components = out_channel.path == GltfAnimationPath::Rotation ? 4u : 3u;

            out_channel.times.resize(times->count);
            if (cgltf_accessor_unpack_floats(times, out_channel.times.data(), times->count) == 0) {
                continue;
            }
            std::vector<float> scratch(values->count * components);
            if (cgltf_accessor_unpack_floats(values, scratch.data(), scratch.size()) == 0) {
                continue;
            }
            out_channel.values.resize(values->count);
            for (cgltf_size k = 0; k < values->count; ++k) {
                out_channel.values[k] = {scratch[k * components + 0], scratch[k * components + 1],
                                         scratch[k * components + 2],
                                         components == 4 ? scratch[k * components + 3] : 0.0f};
            }
            out.duration_seconds = std::max(out.duration_seconds, out_channel.times.back());
            out.channels.push_back(std::move(out_channel));
        }
        model.animations.push_back(std::move(out));
    }

    std::size_t decoded = 0;
    for (const GltfImage& image : model.images) {
        if (image.valid()) {
            ++decoded;
        }
    }
    log::info(
        "glTF '{}': {} nodes, {} meshes, {} materials, {}/{} images decoded, {} skins, {} "
        "animations",
        path.string(), model.nodes.size(), model.meshes.size(), model.materials.size(), decoded,
        model.images.size(), model.skins.size(), model.animations.size());
    return model;
}

}  // namespace eng
