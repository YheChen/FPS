#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>

#include "engine/assets/gltf_loader.h"

namespace eng {

// Path-keyed cache of loaded assets. The cache OWNS all assets; callers get
// stable non-owning pointers valid for the cache's lifetime. Not
// thread-safe (main thread only, like all engine systems).
class AssetCache {
public:
    // `root` is the assets directory (see find_assets_root()).
    explicit AssetCache(std::filesystem::path root);

    // Loads (or returns the cached) model for an asset-relative path like
    // "maps/arena01.glb". Returns nullptr on failure (logged). Failures are
    // not cached, so a fixed file can be retried.
    const GltfModel* model(std::string_view relative_path);

    const std::filesystem::path& root() const { return root_; }

private:
    std::filesystem::path root_;
    std::unordered_map<std::string, std::unique_ptr<GltfModel>> models_;
};

}  // namespace eng
