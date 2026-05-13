#include "file_resolver.h"
#include "whiteout/flakes/types.h"
#include "whiteout/flakes/util/path_utf8.h"

namespace whiteout::flakes::io {
namespace fs = std::filesystem;

static constexpr const char* kTextureExts[] = {".blp", ".dds", ".tga", ".png"};
static constexpr const char* kModelExts[] = {".mdx", ".mdl"};

FileResolver::FileResolver(const fs::path& basePath) : basePath_(basePath) {}

std::string FileResolver::NormalizeSeparators(const std::string& path) {
    std::string out = path;
    for (auto& c : out)
        if (c == '\\')
            c = '/';
    return out;
}

fs::path FileResolver::Resolve(const std::string& relativePath,
                               std::span<const char* const> extensions) const {
    std::string norm = NormalizeSeparators(relativePath);

    fs::path relPath = FsPathFromUtf8(norm);
    fs::path filename = relPath.filename();

    fs::path candidates[4];
    usize nCand = 0;
    if (!basePath_.empty()) {
        candidates[nCand++] = basePath_ / relPath;
        candidates[nCand++] = basePath_ / filename;
    }
    if (!systemBasePath_.empty()) {
        candidates[nCand++] = systemBasePath_ / relPath;
        candidates[nCand++] = systemBasePath_ / filename;
    }

    for (usize i = 0; i < nCand; ++i) {
        const auto& base = candidates[i];

        if (fs::exists(base))
            return base;

        for (const char* ext : extensions) {
            fs::path alt = base;
            alt.replace_extension(ext);
            if (fs::exists(alt))
                return alt;
        }
    }

    return {};
}

fs::path FileResolver::ResolveTexture(const std::string& relativePath) const {
    return Resolve(relativePath, kTextureExts);
}

fs::path FileResolver::ResolveModel(const std::string& relativePath) const {
    return Resolve(relativePath, kModelExts);
}

} // namespace whiteout::flakes::io
