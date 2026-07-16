#include "engine/assets/asset_cache.h"

#include <utility>

#include "engine/assets/paths.h"
#include "engine/core/log.h"

namespace eng {

AssetCache::AssetCache(std::filesystem::path root) : root_(std::move(root)) {}

const GltfModel* AssetCache::model(std::string_view relative_path) {
    const std::string key = normalize_asset_path(relative_path);
    if (asset_path_escapes_root(key)) {
        log::error("Asset path '{}' escapes the asset root; refusing", relative_path);
        return nullptr;
    }

    if (const auto it = models_.find(key); it != models_.end()) {
        return it->second.get();
    }

    auto loaded = load_gltf(root_ / key);
    if (!loaded) {
        return nullptr;
    }
    auto owned = std::make_unique<GltfModel>(std::move(*loaded));
    const GltfModel* raw = owned.get();
    models_.emplace(key, std::move(owned));
    return raw;
}

}  // namespace eng
