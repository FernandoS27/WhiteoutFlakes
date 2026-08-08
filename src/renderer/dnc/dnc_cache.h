#pragma once

#include "dnc_asset.h"
#include "whiteout/flakes/content_provider.h"
#include "whiteout/flakes/types.h"

#include <memory>
#include <string>
#include <unordered_map>

namespace whiteout::flakes::renderer::dnc {

class DncCache {
public:
    explicit DncCache(io::IContentProvider* contentProvider);
    ~DncCache();

    DncCache(const DncCache&) = delete;
    DncCache& operator=(const DncCache&) = delete;

    DncAsset* Acquire(const std::string& path);
    void Release(DncAsset* asset);
    void ReleaseAll();

    /// Re-point at another scene's provider. Drops every entry — they were
    /// resolved through the old provider's mod chain.
    void SetContentProvider(io::IContentProvider* contentProvider);

private:
    // Callers hand in a mod-pinned path when they care about SD vs HD, so the
    // variant is already part of `path` and the key needs nothing extra.
    static std::string NormalizeKey(const std::string& path);
    static bool IsTextPath(const std::string& key);

    io::IContentProvider* contentProvider_ = nullptr;
    std::unordered_map<std::string, std::unique_ptr<DncAsset>> entries_;
};

} // namespace whiteout::flakes::renderer::dnc
