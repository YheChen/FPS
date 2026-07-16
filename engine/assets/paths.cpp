#include "engine/assets/paths.h"

#include <fstream>
#include <sstream>
#include <vector>

#include "engine/core/log.h"

namespace eng {

std::string normalize_asset_path(std::string_view path) {
    std::string slashes(path);
    for (char& c : slashes) {
        if (c == '\\') {
            c = '/';
        }
    }

    std::vector<std::string_view> segments;
    std::string_view rest = slashes;
    while (!rest.empty()) {
        const std::size_t sep = rest.find('/');
        const std::string_view segment = rest.substr(0, sep);
        rest = (sep == std::string_view::npos) ? std::string_view{} : rest.substr(sep + 1);
        if (segment.empty() || segment == ".") {
            continue;
        }
        segments.push_back(segment);
    }

    std::string out;
    for (std::size_t i = 0; i < segments.size(); ++i) {
        if (i > 0) {
            out += '/';
        }
        out += segments[i];
    }
    return out;
}

bool asset_path_escapes_root(std::string_view normalized) {
    std::string_view rest = normalized;
    while (!rest.empty()) {
        const std::size_t sep = rest.find('/');
        const std::string_view segment = rest.substr(0, sep);
        rest = (sep == std::string_view::npos) ? std::string_view{} : rest.substr(sep + 1);
        if (segment == "..") {
            return true;
        }
    }
    return false;
}

std::optional<std::filesystem::path> find_assets_root(const std::filesystem::path& start) {
    std::error_code ec;
    std::filesystem::path dir = std::filesystem::absolute(start, ec);
    if (ec) {
        return std::nullopt;
    }
    for (int depth = 0; depth < 6; ++depth) {
        const std::filesystem::path candidate = dir / "assets";
        if (std::filesystem::is_directory(candidate, ec)) {
            return candidate;
        }
        if (!dir.has_parent_path() || dir.parent_path() == dir) {
            break;
        }
        dir = dir.parent_path();
    }
    return std::nullopt;
}

std::optional<std::string> read_text_file(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        log::error("Failed to open '{}'", path.string());
        return std::nullopt;
    }
    std::ostringstream buffer;
    buffer << file.rdbuf();
    return std::move(buffer).str();
}

}  // namespace eng
