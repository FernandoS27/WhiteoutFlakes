#pragma once

#include "common_types.h"
#include "dnc_asset.h"
#include "io/content_provider.h"

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
    void      Release(DncAsset* asset);
    void      ReleaseAll();

private:
    static std::string NormalizeKey(const std::string& path);
    static bool        IsTextPath(const std::string& key);

    io::IContentProvider* contentProvider_ = nullptr;
    std::unordered_map<std::string, std::unique_ptr<DncAsset>> entries_;
};

}
