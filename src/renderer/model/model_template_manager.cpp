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

ModelTemplateManager::ModelTemplateManager()  = default;
ModelTemplateManager::~ModelTemplateManager() = default;

void ModelTemplateManager::SetContentProvider(IContentProvider* provider) {
    contentProvider_ = provider;
}

void ModelTemplateManager::SetBasePath(std::filesystem::path basePath) {
    basePath_ = std::move(basePath);
}

std::shared_ptr<ModelTemplate> ModelTemplateManager::Lookup(const std::string& mdxPath) {
    std::lock_guard<std::mutex> lock(cacheMutex_);
    auto it = cache_.find(mdxPath);
    if (it == cache_.end()) return nullptr;
    return it->second.lock();
}

std::shared_ptr<ModelTemplate> ModelTemplateManager::GetOrLoadSync(const std::string& mdxPath) {
    {
        std::lock_guard<std::mutex> lock(cacheMutex_);
        auto it = cache_.find(mdxPath);
        if (it != cache_.end()) {
            if (auto live = it->second.lock())
                return live;
            // Weak ref expired (every actor that held it has been
            // destroyed). Drop the dead entry and fall through to a
            // fresh parse.
            cache_.erase(it);
        }
    }
    auto tmpl = ParseAndBuild(mdxPath);
    if (tmpl) {
        std::lock_guard<std::mutex> lock(cacheMutex_);
        cache_[mdxPath] = tmpl;
    }
    return tmpl;
}

std::shared_ptr<ModelTemplate> ModelTemplateManager::Adopt(const std::string& key,
                                                           std::shared_ptr<ModelTemplate> tmpl) {
    std::lock_guard<std::mutex> lock(cacheMutex_);
    cache_[key] = tmpl;
    return tmpl;
}

void ModelTemplateManager::ReleaseAllGPU(gfx::IGFXDevice& gfx) {
    std::lock_guard<std::mutex> lock(cacheMutex_);
    for (auto& [path, weak] : cache_) {
        if (auto tmpl = weak.lock())
            tmpl->ReleaseGPU(gfx);
    }
}

void ModelTemplateManager::Clear() {
    std::lock_guard<std::mutex> lock(cacheMutex_);
    cache_.clear();
}

std::shared_ptr<ModelTemplate> ModelTemplateManager::ParseAndBuild(const std::string& mdxPath) {
    if (!contentProvider_)
        return nullptr;

    // Main-actor MDX bytes are needed in-hand to build the template, so this
    // is an explicit Request + Wait. Compared to ReadFile, the explicit form
    // documents the wait point and lets us write straight into a local
    // rather than allocate a heap-owned shared_ptr<RequestResult>. The
    // callback fires on the host's Pump thread; Wait() returns once it has.
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
    return BuildFromBytes(
        mdxPath,
        std::span<const u8>(fileResult.data.data(), fileResult.data.size()),
        fileResult.actualExt);
}

std::shared_ptr<ModelTemplate> ModelTemplateManager::BuildFromBytes(
    const std::string& mdxPath, std::span<const u8> bytes, std::string_view foundExt) {
    if (bytes.empty())
        return nullptr;

    // The MDX parser also reads the text MDL format from a buffer when told
    // which it is. Pick the format from the resolved extension — `foundExt`
    // is the extension the provider actually delivered (it may swap
    // .mdx<->.mdl), with the request path as a fallback.
    auto endsWithMdl = [](std::string_view s) {
        if (s.size() < 4) return false;
        std::string tail(s.substr(s.size() - 4));
        for (auto& c : tail)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return tail == ".mdl";
    };
    const bool isMdl = !foundExt.empty() ? endsWithMdl(foundExt) : endsWithMdl(mdxPath);
    const whiteout::mdx::MDLXFormat fmt =
        isMdl ? whiteout::mdx::MDLXFormat::MDL : whiteout::mdx::MDLXFormat::MDX;

    whiteout::mdx::Parser mdxParser;
    whiteout::mdx::Model model = mdxParser.parse(
        std::span<const whiteout::u8>(bytes.data(), bytes.size()), fmt);

    namespace fs = std::filesystem;
    fs::path texBasePath = basePath_.empty() ? FsPathFromUtf8(mdxPath).parent_path() : basePath_;

    auto tmpl = std::make_shared<ModelTemplate>();
    auto adapter =
        std::make_shared<MdxModelAdapter>(std::move(model), texBasePath, contentProvider_);

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
