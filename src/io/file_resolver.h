#pragma once

#include "common_types.h"

#include <filesystem>
#include <string>
#include <span>

namespace WhiteoutDex {

class FileResolver {
public:
    explicit FileResolver(const std::filesystem::path& basePath = {});

    void SetBasePath(const std::filesystem::path& basePath) { basePath_ = basePath; }
    const std::filesystem::path& BasePath() const { return basePath_; }

    std::filesystem::path Resolve(const std::string& relativePath,
                                  std::span<const char* const> extensions) const;

    std::filesystem::path ResolveTexture(const std::string& relativePath) const;

    std::filesystem::path ResolveModel(const std::string& relativePath) const;

    static std::string NormalizeSeparators(const std::string& path);

private:
    std::filesystem::path basePath_;
};

}
