#pragma once

#include "whiteout/flakes/types.h"
#include "model/model_template.h"

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

namespace whiteout::flakes::io { class IContentProvider; }
namespace whiteout::flakes::gfx { class IGFXDevice; }

namespace whiteout::flakes::renderer::model {

class ModelTemplateManager {
public:
    using TextureCacheQuery = std::function<bool(std::string_view)>;

    ModelTemplateManager();
    ~ModelTemplateManager();
    ModelTemplateManager(const ModelTemplateManager&)            = delete;
    ModelTemplateManager& operator=(const ModelTemplateManager&) = delete;

    void SetContentProvider(io::IContentProvider* provider);
    void SetBasePath(std::filesystem::path basePath);
    void SetTextureCacheQuery(TextureCacheQuery q);

    std::shared_ptr<ModelTemplate> Lookup(const std::string& mdxPath);

    std::shared_ptr<ModelTemplate> GetOrLoadAsync(const std::string& mdxPath);

    std::shared_ptr<ModelTemplate> GetOrLoadSync(const std::string& mdxPath);

    std::shared_ptr<ModelTemplate> Adopt(const std::string& key,
                                         std::shared_ptr<ModelTemplate>);

    void Tick();

    // Drain the list of templates that finished loading on the worker
    // thread during the most recent Tick() call. The caller (ModelLoader)
    // uploads each one's GPU resources immediately so a later actor
    // spawn referencing the same template doesn't pay the upload cost
    // on its first frame — eliminates one source of first-spawn
    // stutter on PE1-heavy scenes. Returned by move; subsequent calls
    // are empty until the next Tick() picks up another batch.
    std::vector<std::shared_ptr<ModelTemplate>> DrainNewlyLoadedTemplates();

    void ReleaseAllGPU(gfx::IGFXDevice& gfx);
    void Clear();

private:
    void StartLoader();
    void StopLoader();
    void LoaderFunc();

    std::shared_ptr<ModelTemplate> ParseAndBuild(const std::string& mdxPath);

    io::IContentProvider*     contentProvider_ = nullptr;
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

    // Templates that completed loading on the worker thread during
    // the most recent Tick(). Populated under cacheMutex_ (same
    // critical section as the cache write) and drained by
    // DrainNewlyLoadedTemplates(). Single-threaded after Tick() — the
    // render thread is the only consumer.
    std::vector<std::shared_ptr<ModelTemplate>> newlyLoaded_;
};

}
