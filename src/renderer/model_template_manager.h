#pragma once

#include "common_types.h"
#include "model_template.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace WhiteoutDex { class IContentProvider; }
namespace WhiteoutDex::gfx { class IGFXDevice; }

namespace WhiteoutDex {

class ModelTemplateManager {
public:
    using TextureCacheQuery = std::function<bool(std::string_view)>;

    ModelTemplateManager();
    ~ModelTemplateManager();
    ModelTemplateManager(const ModelTemplateManager&)            = delete;
    ModelTemplateManager& operator=(const ModelTemplateManager&) = delete;

    void SetContentProvider(IContentProvider* provider);
    void SetBasePath(std::filesystem::path basePath);
    void SetTextureCacheQuery(TextureCacheQuery q);

    std::shared_ptr<ModelTemplate> Lookup(const std::string& mdxPath);

    std::shared_ptr<ModelTemplate> GetOrLoadAsync(const std::string& mdxPath);

    std::shared_ptr<ModelTemplate> GetOrLoadSync(const std::string& mdxPath);

    std::shared_ptr<ModelTemplate> Adopt(const std::string& key,
                                         std::shared_ptr<ModelTemplate>);

    void Tick();

    void ReleaseAllGPU(gfx::IGFXDevice& gfx);
    void Clear();

private:
    void StartLoader();
    void StopLoader();
    void LoaderFunc();

    std::shared_ptr<ModelTemplate> ParseAndBuild(const std::string& mdxPath);

    IContentProvider*     contentProvider_ = nullptr;
    std::filesystem::path basePath_;
    TextureCacheQuery     textureCacheQuery_;

    mutable std::mutex                                              cacheMutex_;
    std::unordered_map<std::string, std::shared_ptr<ModelTemplate>> cache_;

    std::thread                     loaderThread_;
    std::atomic<bool>               loaderRunning_{false};
    std::mutex                      queueMutex_;
    std::condition_variable         queueCV_;
    std::deque<std::string>         loadQueue_;
    std::unordered_set<std::string> loadPending_;
    std::mutex                      resultMutex_;
    std::vector<std::pair<std::string, std::shared_ptr<ModelTemplate>>> loadResults_;
};

}
