#include "file_resolver.h"
#include "common_types.h"
#include "io/path_utf8.h"

namespace whiteout::flakes::io {
namespace fs = std::filesystem;

static constexpr const char* kTextureExts[] = {".blp", ".dds", ".tga", ".png"};
static constexpr const char* kModelExts[]   = {".mdx", ".mdl"};

FileResolver::FileResolver(const fs::path& basePath)
    : basePath_(basePath) {}

std::string FileResolver::NormalizeSeparators(const std::string& path) {
    std::string out = path;
    for (auto& c : out)
        if (c == '\\') c = '/';
    return out;
}

fs::path FileResolver::Resolve(const std::string& relativePath,
                                std::span<const char* const> extensions) const {
    std::string norm = NormalizeSeparators(relativePath);

    fs::path relPath = FsPathFromUtf8(norm);
    fs::path filename = relPath.filename();

    const fs::path candidates[] = {
        basePath_ / relPath,
        basePath_ / filename,
    };

    for (const auto& base : candidates) {

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

}
