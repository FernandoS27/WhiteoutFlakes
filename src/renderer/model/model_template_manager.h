#pragma once

#include "model/model_template.h"
#include "whiteout/flakes/types.h"

#include <filesystem>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>

namespace whiteout::flakes::io {
class IContentProvider;
}
namespace whiteout::flakes::gfx {
class IGFXDevice;
}

namespace whiteout::flakes::renderer::model {

class ModelTemplateManager {
public:
    ModelTemplateManager();
    ~ModelTemplateManager();
    ModelTemplateManager(const ModelTemplateManager&) = delete;
    ModelTemplateManager& operator=(const ModelTemplateManager&) = delete;

    void SetContentProvider(io::IContentProvider* provider);
    void SetBasePath(std::filesystem::path basePath);

    std::shared_ptr<ModelTemplate> Lookup(const std::string& mdxPath);

    std::shared_ptr<ModelTemplate> GetOrLoadSync(const std::string& mdxPath);

    /// @brief Build a template from already-fetched bytes — used by
    ///        AssetManager's ChildModel pipeline. The path is only
    ///        used as a hint (sets the adapter's texBasePath + picks
    ///        MDX vs MDL format from the extension); the bytes are
    ///        the actual source. Returns nullptr on parse failure.
    std::shared_ptr<ModelTemplate> BuildFromBytes(const std::string& path,
                                                  std::span<const u8> bytes,
                                                  std::string_view foundExt);

    std::shared_ptr<ModelTemplate> Adopt(const std::string& key, std::shared_ptr<ModelTemplate>);

    void ReleaseAllGPU(gfx::IGFXDevice& gfx);
    void Clear();

private:
    std::shared_ptr<ModelTemplate> ParseAndBuild(const std::string& mdxPath);

    io::IContentProvider* contentProvider_ = nullptr;
    std::filesystem::path basePath_;

    mutable std::mutex cacheMutex_;
    // Weak-ref cache: a re-Spawn of the same MDX hits the cached
    // template if any actor still holds it via sourceTemplate, but
    // the cache itself doesn't pin the template alive. Once every
    // actor referencing a model is destroyed the template (and its
    // GPU geosets) reclaims; loading a new model afterwards re-
    // parses cleanly. Replaces an earlier shared_ptr-keyed cache
    // that grew unbounded across model switches.
    std::unordered_map<std::string, std::weak_ptr<ModelTemplate>> cache_;
};

} // namespace whiteout::flakes::renderer::model
