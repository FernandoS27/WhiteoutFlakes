#pragma once

#include "whiteout/flakes/types.h"

#include <filesystem>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace whiteout::flakes::io {

class FileResolver {
public:
    explicit FileResolver(const std::filesystem::path& basePath = {});

    void SetBasePath(const std::filesystem::path& basePath) {
        basePath_ = basePath;
        ClearListings();
    }
    const std::filesystem::path& BasePath() const {
        return basePath_;
    }

    // Secondary lookup path checked AFTER basePath_. Used by the host to
    // surface engine-shipped assets (shaders, etc.) that live next to the
    // executable rather than alongside the loaded model.
    void SetSystemBasePath(const std::filesystem::path& p) {
        systemBasePath_ = p;
        ClearListings();
    }
    const std::filesystem::path& SystemBasePath() const {
        return systemBasePath_;
    }

    // Roots a relative path is also tried under, after the base path and
    // before the system one: the asset root of the mod a loose model ships in.
    void SetAssetRoots(std::vector<std::filesystem::path> roots) {
        assetRoots_ = std::move(roots);
        ClearListings();
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
    struct DirEntry {
        std::string lower; ///< ASCII-lowercased name, what a lookup compares
        std::string name;  ///< the name as it is on disk
        bool isDirectory = false;
    };
    using Listing = std::vector<DirEntry>;

    // Directory listings for case-insensitive matching, kept until the roots
    // change: every miss walks them, and a model load misses thousands of
    // times (each read that lands in an archive asks the disk first). A match
    // is still checked on disk before it is returned, so a deleted file is not
    // an answer; a file added after its folder was listed is found by an
    // exact-case lookup, and a differently-cased one waits for the next root
    // change. A copy of the resolver starts with an empty cache.
    struct ListingCache {
        ListingCache() = default;
        ListingCache(const ListingCache&) {}
        ListingCache& operator=(const ListingCache&) {
            std::lock_guard lk(mu);
            map.clear();
            return *this;
        }
        std::mutex mu;
        std::unordered_map<std::string, std::shared_ptr<const Listing>> map;
    };

    void ClearListings();
    std::shared_ptr<const Listing> ListDirectory(const std::filesystem::path& dir) const;
    std::filesystem::path ResolveCaseInsensitive(const std::filesystem::path& root, const std::filesystem::path& rel,
                                                 std::span<const char* const> extensions) const;

    std::filesystem::path basePath_;
    std::filesystem::path systemBasePath_;
    std::vector<std::filesystem::path> assetRoots_;
    mutable ListingCache listings_;
};

} // namespace whiteout::flakes::io
