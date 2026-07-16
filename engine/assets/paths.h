#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace eng {

// Canonicalizes an asset-relative path: backslashes to slashes, leading
// "./" and "/" stripped, "." segments removed. Does NOT touch case and does
// not resolve "..", which is rejected by asset loading (no escaping the
// asset root). Pure function.
std::string normalize_asset_path(std::string_view path);

// True if the normalized path tries to escape the asset root ("..").
bool asset_path_escapes_root(std::string_view normalized);

// Finds the "assets/" directory by walking up from `start` (at most 6
// levels). Lets binaries run from the repo root, build/, build/debug/game/,
// etc.
std::optional<std::filesystem::path> find_assets_root(
    const std::filesystem::path& start = std::filesystem::current_path());

// Reads an entire file as text. nullopt (with an error log) on failure.
std::optional<std::string> read_text_file(const std::filesystem::path& path);

}  // namespace eng
