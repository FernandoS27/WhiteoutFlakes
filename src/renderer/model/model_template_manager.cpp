#include "renderer/model/model_template_manager.h"

#include <whiteout/models/mdx/parser.h>
#include "gfx/gfx.h"
#include "io/mdx_model_adapter.h"
#include "renderer/animation/animation.h"
#include "whiteout/flakes/content_provider.h"
#include "whiteout/flakes/util/path_utf8.h"

#include <cctype>
#include <chrono>
#include <cstdio>
#include <span>
#include <string>
#include <utility>

namespace whiteout::flakes::renderer::model {

using namespace ::whiteout::flakes::renderer::animation;
using namespace ::whiteout::flakes::io;

ModelTemplateManager::ModelTemplateManager() {
#if !defined(__EMSCRIPTEN__)
    // Single-threaded WASM has no std::thread; ctor would throw "Not
    // supported". GetOrLoadSync runs ParseAndBuild on the calling thread
    // already, so spawning skips cleanly.
    StartLoader();
#endif
}

ModelTemplateManager::~ModelTemplateManager() {
#if !defined(__EMSCRIPTEN__)
    StopLoader();
#endif
}

void ModelTemplateManager::SetContentProvider(IContentProvider* provider) {
    contentProvider_ = provider;
}

void ModelTemplateManager::SetBasePath(std::filesystem::path basePath) {
    basePath_ = std::move(basePath);
}

void ModelTemplateManager::SetTextureCacheQuery(TextureCacheQuery q) {
    textureCacheQuery_ = std::move(q);
}

std::shared_ptr<ModelTemplate> ModelTemplateManager::Lookup(const std::string& mdxPath) {
    std::lock_guard<std::mutex> lock(cacheMutex_);
    auto it = cache_.find(mdxPath);
    return (it != cache_.end()) ? it->second : nullptr;
}

std::shared_ptr<ModelTemplate> ModelTemplateManager::GetOrLoadAsync(const std::string& mdxPath) {
    {
        std::lock_guard<std::mutex> lock(cacheMutex_);
        auto it = cache_.find(mdxPath);
        // Under EMSCRIPTEN the loader-thread Tick never runs, so a cached
        // null would pin a failed-once template forever — re-try on the
        // calling thread instead. The JS lazy drain ferries the missing
        // bytes into the content provider as cold paths surface, so the
        // next frame's attempt parses cleanly. On desktop the worker thread
        // owns retries, so we honour the cached null there.
#if defined(__EMSCRIPTEN__)
        if (it != cache_.end() && it->second)
            return it->second;
#else
        if (it != cache_.end())
            return it->second;
#endif
    }
#if defined(__EMSCRIPTEN__)
    // Web build has no loader thread (see ctor). ParseAndBuild is safe to
    // call from the render thread because the FetchContentProvider's
    // Request resolves synchronously against its in-memory cache — Wait()
    // never blocks. A null return here just means "bytes not yet Put"; the
    // caller (frame_ticker) skips the child this frame, JS drains the
    // miss, and the next frame the template builds.
    auto tmpl = ParseAndBuild(mdxPath);
    if (tmpl) {
        std::lock_guard<std::mutex> lock(cacheMutex_);
        cache_[mdxPath] = tmpl;
    }
    return tmpl;
#else
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        if (loadPending_.count(mdxPath))
            return nullptr;
        loadPending_.insert(mdxPath);
        loadQueue_.push_back(mdxPath);
    }
    queueCV_.notify_one();
    return nullptr;
#endif
}

std::shared_ptr<ModelTemplate> ModelTemplateManager::GetOrLoadSync(const std::string& mdxPath) {

    {
        std::lock_guard<std::mutex> lock(cacheMutex_);
        auto it = cache_.find(mdxPath);
        if (it != cache_.end())
            return it->second;
    }

    auto tmpl = ParseAndBuild(mdxPath);
    {
        std::lock_guard<std::mutex> lock(cacheMutex_);
        cache_[mdxPath] = tmpl;
    }
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        loadPending_.erase(mdxPath);
    }
    return tmpl;
}

std::shared_ptr<ModelTemplate> ModelTemplateManager::Adopt(const std::string& key,
                                                           std::shared_ptr<ModelTemplate> tmpl) {
    std::lock_guard<std::mutex> lock(cacheMutex_);
    cache_[key] = tmpl;
    return tmpl;
}

void ModelTemplateManager::Tick() {
    std::vector<std::pair<std::string, std::shared_ptr<ModelTemplate>>> results;
    {
        std::lock_guard<std::mutex> lock(resultMutex_);
        results.swap(loadResults_);
    }
    if (results.empty())
        return;
    {
        std::lock_guard<std::mutex> lock(cacheMutex_);
        for (auto& [path, tmpl] : results)
            cache_[path] = tmpl;
    }
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        for (auto& [path, _] : results)
            loadPending_.erase(path);
    }
    // Stash the just-finished templates so the renderer can pre-upload
    // their GPU resources before any actor spawns request them.
    newlyLoaded_.reserve(newlyLoaded_.size() + results.size());
    for (auto& [path, tmpl] : results) {
        if (tmpl)
            newlyLoaded_.push_back(tmpl);
    }
}

std::vector<std::shared_ptr<ModelTemplate>> ModelTemplateManager::DrainNewlyLoadedTemplates() {
    std::vector<std::shared_ptr<ModelTemplate>> out;
    out.swap(newlyLoaded_);
    return out;
}

void ModelTemplateManager::ReleaseAllGPU(gfx::IGFXDevice& gfx) {
    std::lock_guard<std::mutex> lock(cacheMutex_);
    for (auto& [path, tmpl] : cache_) {
        if (tmpl)
            tmpl->ReleaseGPU(gfx);
    }
}

void ModelTemplateManager::Clear() {
    std::lock_guard<std::mutex> lock(cacheMutex_);
    cache_.clear();
}

void ModelTemplateManager::StartLoader() {
    loaderRunning_ = true;
    loaderThread_ = std::thread(&ModelTemplateManager::LoaderFunc, this);
}

void ModelTemplateManager::StopLoader() {
    loaderRunning_ = false;
    queueCV_.notify_one();
    if (loaderThread_.joinable())
        loaderThread_.join();
}

void ModelTemplateManager::LoaderFunc() {
    while (loaderRunning_) {
        std::string path;
        {
            std::unique_lock<std::mutex> lock(queueMutex_);
            queueCV_.wait_for(lock, std::chrono::milliseconds(50),
                              [this] { return !loadQueue_.empty() || !loaderRunning_; });
            if (!loaderRunning_)
                break;
            if (loadQueue_.empty())
                continue;
            path = std::move(loadQueue_.front());
            loadQueue_.pop_front();
        }

        auto tmpl = ParseAndBuild(path);

        {
            std::lock_guard<std::mutex> lock(resultMutex_);
            loadResults_.emplace_back(std::move(path), std::move(tmpl));
        }
    }
}

std::shared_ptr<ModelTemplate> ModelTemplateManager::ParseAndBuild(const std::string& mdxPath) {
    if (!contentProvider_)
        return nullptr;

    // Main-actor MDX bytes are needed in-hand to build the template, so this
    // is an explicit Request + Wait. Compared to ReadFile, the explicit form
    // documents the wait point and lets us write straight into a local rather
    // than allocating a heap-owned shared_ptr<RequestResult>. The callback
    // fires on the host's Pump thread; doneCv wakes us once it returns.
    io::RequestResult fileResult;
    const io::RequestId reqId = contentProvider_->Request(
        mdxPath, [&fileResult](io::RequestResult&& r) { fileResult = std::move(r); });
    if (reqId == io::kInvalidRequestId) {
        std::fprintf(stderr, "[model] ERR: MDX request rejected '%s'\n", mdxPath.c_str());
        return nullptr;
    }
    contentProvider_->Wait(reqId);
    if (!fileResult.ok || fileResult.data.empty()) {
        std::fprintf(stderr, "[model] ERR: MDX read FAIL '%s'\n", mdxPath.c_str());
        return nullptr;
    }

    // The MDX parser also reads the text MDL format from a buffer when told
    // which it is. Pick the format from the resolved extension — actualExt is
    // the file the provider actually returned (it may swap .mdx<->.mdl), with
    // the request path as a fallback when the provider left it blank.
    const std::string& extSrc = !fileResult.actualExt.empty() ? fileResult.actualExt : mdxPath;
    auto endsWithMdl = [](const std::string& s) {
        if (s.size() < 4)
            return false;
        std::string tail = s.substr(s.size() - 4);
        for (auto& c : tail)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return tail == ".mdl";
    };
    const whiteout::mdx::MDLXFormat fmt =
        endsWithMdl(extSrc) ? whiteout::mdx::MDLXFormat::MDL : whiteout::mdx::MDLXFormat::MDX;

    whiteout::mdx::Parser mdxParser;
    whiteout::mdx::Model model;
    // WhiteoutLib's MDX parser is now no-throw; parse errors surface via
    // its issues vector (checked downstream). The previous try/catch
    // would also be rejected under the web build's -fno-exceptions.
    model = mdxParser.parse(
        std::span<const whiteout::u8>(fileResult.data.data(), fileResult.data.size()), fmt);

    namespace fs = std::filesystem;

    fs::path texBasePath = basePath_.empty() ? FsPathFromUtf8(mdxPath).parent_path() : basePath_;

    auto tmpl = std::make_shared<ModelTemplate>();
    auto adapter =
        std::make_shared<MdxModelAdapter>(std::move(model), texBasePath, contentProvider_);
    if (textureCacheQuery_)
        adapter->SetTextureCacheQuery(textureCacheQuery_);

    tmpl->adapter = adapter;
    tmpl->meshes = adapter->GetMeshes();
    tmpl->textures = adapter->GetTextures();
    tmpl->materials = adapter->GetMaterials();
    tmpl->skeleton = adapter->GetSkeleton();
    tmpl->skinWeights = adapter->GetSkinWeights();
    tmpl->pe2Configs = adapter->GetParticleConfigs();
    tmpl->ribbonConfigs = adapter->GetRibbonConfigs();
    tmpl->collisionConfigs = adapter->GetCollisionShapes();
    tmpl->pe1Configs = adapter->GetPE1Configs();
    tmpl->cornEmitterInits = adapter->GetCornEmitterInits();
    tmpl->attachmentConfigs = adapter->GetAttachmentConfigs();
    tmpl->eventObjects = adapter->GetEventObjects();
    tmpl->globalSequences = adapter->GetGlobalSequences();
    tmpl->cameraPresets = adapter->GetCameraPresets();

    // Decide per-actor palette path (Path A vs B). On Path A this
    // rewrites every vertex's boneIdx in `tmpl->skinWeights` to a
    // global slot index so the GPU vertex buffer (built downstream
    // from the same data) is consistent with the per-actor palette
    // the renderer will fill. Must run before the geosetWeights
    // copy below so the rewritten values land in SkinningData.
    auto paletteDecision =
        animation::DecidePaletteLayoutAndRewrite(tmpl->skeleton.nodeCount, tmpl->skinWeights);

    auto skinningData = std::make_shared<SkinningData>();
    skinningData->nodeCount = tmpl->skeleton.nodeCount;
    skinningData->inverseBindMatrices = tmpl->skeleton.inverseBindMatrices;
    skinningData->actorPaletteSize = paletteDecision.actorPaletteSize;
    skinningData->usesPerActorPalette = paletteDecision.usesPerActorPalette;
    skinningData->globalGroupAverages = std::move(paletteDecision.globalGroupAverages);
    for (auto& sw : tmpl->skinWeights) {
        const i32 vc = (i32)sw.influences.size();
        auto& info = skinningData->geosetWeights[sw.geosetId];
        info.vertices.resize(vc);
        for (i32 v = 0; v < vc; v++) {
            for (i32 j = 0; j < 4; j++) {
                info.vertices[v].boneIdx[j] = sw.influences[v].boneIdx[j];
                info.vertices[v].weight[j] = sw.influences[v].weight[j];
            }
        }
        GeosetPaletteLayout layout;
        layout.subsetNodeIndices = sw.subsetNodeIndices;
        layout.groupAverages = sw.groupAverages;
        skinningData->geosetLayouts[sw.geosetId] = std::move(layout);
    }
    tmpl->skinningData = std::move(skinningData);

    return tmpl;
}

} // namespace whiteout::flakes::renderer::model
