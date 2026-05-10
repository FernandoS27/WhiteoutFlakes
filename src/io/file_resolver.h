#pragma once

#include "whiteout/flakes/types.h"

#include <filesystem>
#include <string>
#include <span>

namespace whiteout::flakes::io {

class FileResolver {
public:
    explicit FileResolver(const std::filesystem::path& basePath = {});

    void SetBasePath(const std::filesystem::path& basePath) { basePath_ = basePath; }
    const std::filesystem::path& BasePath() const { return basePath_; }

    // Secondary lookup path checked AFTER basePath_. Used by the host to
    // surface engine-shipped assets (shaders, etc.) that live next to the
    // executable rather than alongside the loaded model.
    void SetSystemBasePath(const std::filesystem::path& p) { systemBasePath_ = p; }
    const std::filesystem::path& SystemBasePath() const { return systemBasePath_; }

    std::filesystem::path Resolve(const std::string& relativePath,
                                  std::span<const char* const> extensions) const;

    std::filesystem::path ResolveTexture(const std::string& relativePath) const;

    std::filesystem::path ResolveModel(const std::string& relativePath) const;

    static std::string NormalizeSeparators(const std::string& path);

private:
    std::filesystem::path basePath_;
    std::filesystem::path systemBasePath_;
};

}
