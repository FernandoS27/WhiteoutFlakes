#pragma once

#include "whiteout/flakes/types.h"

#include <filesystem>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace whiteout::flakes::io {

class FileResolver {
public:
    explicit FileResolver(const std::filesystem::path& basePath = {});

    void SetBasePath(const std::filesystem::path& basePath) {
        basePath_ = basePath;
    }
    const std::filesystem::path& BasePath() const {
        return basePath_;
    }

    // Secondary lookup path checked AFTER basePath_. Used by the host to
    // surface engine-shipped assets (shaders, etc.) that live next to the
    // executable rather than alongside the loaded model.
    void SetSystemBasePath(const std::filesystem::path& p) {
        systemBasePath_ = p;
    }
    const std::filesystem::path& SystemBasePath() const {
        return systemBasePath_;
    }

    // Roots a relative path is also tried under, after the base path and
    // before the system one: the asset root of the mod a loose model ships in.
    void SetAssetRoots(std::vector<std::filesystem::path> roots) {
        assetRoots_ = std::move(roots);
    }
    const std::vector<std::filesystem::path>& AssetRoots() const {
        return assetRoots_;
    }

    std::filesystem::path Resolve(const std::string& relativePath,
                                  std::span<const char* const> extensions) const;

    std::filesystem::path ResolveTexture(const std::string& relativePath) const;

    std::filesystem::path ResolveModel(const std::string& relativePath) const;

    static std::string NormalizeSeparators(const std::string& path);

private:
    std::filesystem::path basePath_;
    std::filesystem::path systemBasePath_;
    std::vector<std::filesystem::path> assetRoots_;
};

} // namespace whiteout::flakes::io
