#include "storage_paths.h"

#include <array>
#include <cctype>
#include <filesystem>

namespace whiteout::flakes::io {

namespace {

namespace fs = std::filesystem;

constexpr std::array<const char*, 5> kTextureExts = {".blp", ".dds", ".tga", ".png", ".tif"};
constexpr std::array<const char*, 2> kModelExts = {".mdx", ".mdl"};

bool HasExtension(const std::string& ext, const char* const* list, usize count) {
    for (usize i = 0; i < count; ++i)
        if (ext == list[i])
            return true;
    return false;
}

char Lower(char c) {
    return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
}

} // namespace

std::string NormalizeCascPath(std::string_view relPath) {
    std::string out;
    out.reserve(relPath.size());
    for (char c : relPath)
        out += Lower(c == '/' ? '\\' : c);
    usize start = 0;
    while (start < out.size() && (out[start] == '\\' || out[start] == '/'))
        ++start;
    if (start > 0)
        out.erase(0, start);
    return out;
}

std::string ToListingPath(std::string_view stored) {
    const auto colon = stored.rfind(':');
    if (colon != std::string_view::npos)
        stored.remove_prefix(colon + 1);
    std::string out;
    out.reserve(stored.size());
    for (char c : stored)
        out += Lower(c == '\\' ? '/' : c);
    usize start = 0;
    while (start < out.size() && out[start] == '/')
        ++start;
    if (start > 0)
        out.erase(0, start);
    return out;
}

std::string NormalizeListingDir(const std::string& directory) {
    std::string dir = ToListingPath(directory);
    while (!dir.empty() && dir.back() == '/')
        dir.pop_back();
    return dir;
}

bool MatchesListingDir(const std::string& relPath, const std::string& dir, bool recursive) {
    usize offset = 0;
    if (!dir.empty()) {
        if (relPath.size() <= dir.size() + 1 || relPath.compare(0, dir.size(), dir) != 0 ||
            relPath[dir.size()] != '/')
            return false;
        offset = dir.size() + 1;
    }
    return recursive || relPath.find('/', offset) == std::string::npos;
}

std::string StripExtension(std::string_view path) {
    const auto dot = path.rfind('.');
    return std::string(dot != std::string_view::npos ? path.substr(0, dot) : path);
}

std::string GetLowerExtension(std::string_view relPath) {
    std::string ext = fs::path(relPath).extension().string();
    for (auto& c : ext)
        c = Lower(c);
    return ext;
}

std::pair<const char* const*, usize> AltExtensionsFor(const std::string& ext) {
    if (HasExtension(ext, kTextureExts.data(), kTextureExts.size()))
        return {kTextureExts.data(), kTextureExts.size()};
    if (HasExtension(ext, kModelExts.data(), kModelExts.size()))
        return {kModelExts.data(), kModelExts.size()};
    return {nullptr, 0};
}

AssetKind ClassifyByExtension(const std::string& ext) {
    if (HasExtension(ext, kTextureExts.data(), kTextureExts.size()))
        return AssetKind::Texture;
    if (HasExtension(ext, kModelExts.data(), kModelExts.size()))
        return AssetKind::Model;
    return AssetKind::Other;
}

} // namespace whiteout::flakes::io
