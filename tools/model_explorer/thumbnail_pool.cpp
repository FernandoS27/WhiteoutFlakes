#include "thumbnail_pool.h"

#include "pkb_effect_source.h" // PkbEffectSource (from tools/basic_viewer)
#include "thumbnail_framing.h"

#include "renderer/camera.h"
#include "renderer/frame_ticker.h"
#include "renderer/model/model_instance.h"
#include "renderer/model/model_loader.h"
#include "renderer/render_pipeline.h"
#include "renderer/render_settings.h"
#include "renderer/scene_manager.h"
#include "renderer/viewport.h"

#include "whiteout/flakes/content_provider.h"
#include "whiteout/flakes/util/path_utf8.h"

#include <algorithm>
#include <cctype>
#include <filesystem>

namespace whiteout::flakes::tools {

using renderer::Camera;
using renderer::SceneId;

ThumbnailPool::ThumbnailPool(renderer::RenderService& svc,
                             std::shared_ptr<io::IContentProvider> provider, int cap,
                             int resolution)
    : svc_(svc), provider_(std::move(provider)), cap_(cap), res_(resolution) {}

ThumbnailPool::~ThumbnailPool() {
    Clear();
}

void ThumbnailPool::BeginFrame(std::uint64_t) {
    for (auto& c : cells_)
        c->visible = false;
}

void ThumbnailPool::SetupScene(SceneId scene) {
    auto& sm = svc_.SceneAt(scene);
    // All cell scenes read files from the one shared CASC-backed provider.
    sm.SetContentProvider(provider_);
    auto& cam = sm.Camera();
    cam.SetOrbitalMode();
    cam.SetYaw(Camera::kDefaultYaw - 0.785398f);
    cam.SetPitch(0.6f);
    cam.SetDistance(150.0f);
    cam.SetFovDiagonal(Camera::kDefaultFovDiagonal);
    cam.SetClip(Camera::kDefaultNearZ, Camera::kDefaultFarZ);
}

ThumbnailPool::Cell* ThumbnailPool::AcquireSlot(const std::string& path, bool isEffect,
                                                std::uint64_t frameId) {
    if (auto it = byPath_.find(path); it != byPath_.end()) {
        it->second->visible = true;
        it->second->lastVisibleFrame = frameId;
        return it->second;
    }

    // Need a new cell. Reuse a free slot under the cap, else recycle the
    // least-recently-visible cell that isn't visible this frame.
    Cell* slot = nullptr;
    if (static_cast<int>(cells_.size()) < cap_) {
        cells_.push_back(std::make_unique<Cell>());
        slot = cells_.back().get();
        slot->scene = svc_.CreateScene();
        slot->target = svc_.Pipeline().CreateOffscreenTarget(res_, res_);
        SetupScene(slot->scene);
    } else {
        std::uint64_t oldest = ~0ull;
        for (auto& c : cells_) {
            if (!c->visible && c->lastVisibleFrame < oldest) {
                oldest = c->lastVisibleFrame;
                slot = c.get();
            }
        }
        if (!slot)
            return nullptr; // every cell is visible — caller draws a placeholder
        // Recycle: drop the old model by recreating the scene (full reset of
        // its actors + effect emitters); keep the target. Wait for the GPU to
        // finish with the old scene first (its target was sampled last frame).
        if (auto* gfx = svc_.Pipeline().Gfx())
            gfx->WaitIdle();
        byPath_.erase(slot->path);
        svc_.DestroyScene(slot->scene);
        slot->scene = svc_.CreateScene();
        SetupScene(slot->scene);
    }

    slot->path = path;
    slot->isEffect = isEffect;
    slot->actor = 0;
    slot->loaded = false;
    slot->effectWarmup = -1;
    slot->visible = true;
    slot->lastVisibleFrame = frameId;
    byPath_[path] = slot;
    return slot;
}

void ThumbnailPool::LoadCell(Cell& cell) {
    svc_.SetActiveScene(cell.scene);
    auto& sm = svc_.SceneAt(cell.scene);

    // PE1 child resolution searches relative to the model's archive folder.
    std::filesystem::path archivePath(cell.path);
    sm.SetPE1BasePath(archivePath.parent_path());

    renderer::model::Actor* hero = nullptr;
    if (cell.isEffect) {
        // A standalone effect has no material layers to probe, so derive its
        // HD-ness from its archive location: an `_hd.w3mod` .pkb references HD
        // particle textures and must load + render in HD, exactly as it would
        // when attached to an HD model.
        cell.isHd = cell.path.find("_hd.w3mod") != std::string::npos;
        auto src = std::make_shared<renderer::model::PkbEffectSource>(cell.path);
        hero = svc_.Loader().SpawnUnitFromSource(src);
        cell.effectWarmup = 0; // frame once particles develop
    } else {
        hero = svc_.Loader().SpawnUnit(cell.path);
        if (hero) {
            cell.isHd = tools::IsHdModel(hero);
            // Play the stand animation (or the first if there's no stand).
            const int idx = tools::PickStandSequenceIndex(hero);
            if (idx >= 0)
                hero->animation.SetActiveSequenceIndex(idx);
            // A "portrait" model with an embedded camera frames the face the way
            // the artist intended — use it. Everything else auto-frames to the
            // active animation's extents.
            std::string lower = cell.path;
            for (char& c : lower)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            const bool isPortrait = lower.find("portrait") != std::string::npos;
            if (!isPortrait || !tools::ApplyModelCamera(sm.Camera(), hero)) {
                const auto seqs = hero->animation.Sequences();
                const std::string seqName = (idx >= 0 && idx < static_cast<int>(seqs.size()))
                                                ? seqs[idx].name
                                                : std::string();
                tools::FrameCameraToModelSequence(sm.Camera(), hero, seqName);
            }
        }
    }
    cell.actor = hero ? hero->handle : 0;
    cell.loaded = true;
    svc_.SetActiveScene(svc_.DefaultSceneId());
}

void ThumbnailPool::ResetEffect(Cell& cell) {
    // Most standalone .pkb effects are one-shot bursts (a fireball, an impact)
    // that develop, peak, then die — leaving a dead thumbnail. Re-spawn the
    // emitter periodically so the cell keeps replaying. DestroyActor releases
    // the old corn/particle state; the camera stays where the first warm-up
    // framed it (effectWarmup left at -1) so the loop doesn't visibly re-pan.
    svc_.SetActiveScene(cell.scene);
    if (cell.actor)
        svc_.Loader().DestroyActor(cell.actor);
    auto src = std::make_shared<renderer::model::PkbEffectSource>(cell.path);
    auto* hero = svc_.Loader().SpawnUnitFromSource(src);
    cell.actor = hero ? hero->handle : 0;
    svc_.SetActiveScene(svc_.DefaultSceneId());
}

void ThumbnailPool::DestroyCell(Cell& cell) {
    if (cell.target)
        svc_.Pipeline().DestroyTarget(cell.target);
    if (cell.scene)
        svc_.DestroyScene(cell.scene);
    cell.target = 0;
    cell.scene = 0;
}

gfx::TextureHandle ThumbnailPool::Acquire(const std::string& path, bool isEffect,
                                          std::uint64_t frameId) {
    Cell* cell = AcquireSlot(path, isEffect, frameId);
    if (!cell)
        return gfx::TextureHandle::Invalid;
    if (!cell->loaded)
        return gfx::TextureHandle::Invalid; // loaded + first render happen in RenderVisible
    return svc_.Pipeline().GetTargetColorTexture(cell->target);
}

void ThumbnailPool::RenderVisible(float dt) {
    constexpr int kEffectWarmupTicks = 12;
    constexpr int kEffectMaxTicks = 90;
    constexpr float kEffectResetSeconds = 5.0f;
    for (auto& cptr : cells_) {
        Cell& cell = *cptr;
        if (!cell.visible)
            continue;
        if (!cell.loaded)
            LoadCell(cell);

        // Each cell renders in its own NATURAL shading mode: HD Reforged
        // models / HD effects through the HD pipeline, classic SD models
        // through the SD pipeline. SceneHdrInSd (enabled once below) routes the
        // SD path through the same HDR scene target + tonemap as HD, so SD
        // additive / team-color geosets (water foam, team banners, propeller
        // blur — the clipping the standalone .pkb effects hit) roll off instead
        // of clipping to opaque white, while keeping authentic SD shading. The
        // provider texture overlay follows the model's real HD-ness so SD
        // models keep their SD textures. Cells render sequentially, so a single
        // global setting is correct per cell.
        svc_.Settings().SetSceneHdrInSd(true);
        svc_.Settings().SetRenderMode(cell.isHd ? renderer::RenderMode::HD
                                                : renderer::RenderMode::SD);
        if (provider_)
            provider_->SetHdMode(cell.isHd);

        // Loop standalone effects: re-spawn every few seconds so one-shot
        // bursts keep replaying instead of developing once and dying. Done with
        // the cell's provider/render mode already set so the respawn resolves
        // its textures correctly.
        if (cell.isEffect && cell.loaded) {
            cell.effectAge += dt;
            if (cell.effectAge >= kEffectResetSeconds) {
                cell.effectAge = 0.0f;
                ResetEffect(cell);
            }
        }

        auto& sm = svc_.SceneAt(cell.scene);
        sm.Update(dt); // advance the scene clock so the stand animation actually plays
        svc_.Ticker().Tick(sm, dt);

        // Deferred effect framing once the particle cloud develops.
        if (cell.effectWarmup >= 0) {
            ++cell.effectWarmup;
            if (cell.effectWarmup >= kEffectWarmupTicks) {
                svc_.SetActiveScene(cell.scene);
                bool framed = tools::FrameCameraToEffect(svc_, sm.Camera(), cell.actor);
                svc_.SetActiveScene(svc_.DefaultSceneId());
                if (framed || cell.effectWarmup >= kEffectMaxTicks)
                    cell.effectWarmup = -1;
            }
        }

        renderer::Viewport vp;
        vp.scene = cell.scene;
        vp.target = cell.target;
        vp.camera = &sm.Camera();
        vp.drawImGui = false; // a thumbnail must NOT redraw the host UI on top of itself
        svc_.Pipeline().RenderViewport(vp);
        // Present the offscreen target so its work is submitted to the GPU. For
        // a headless target this is just SubmitFrame (no swap-chain present);
        // without it the deferred-submit backends (Vulkan/WebGPU) never flush
        // the cell's render, so the main ImGui pass samples an empty texture.
        svc_.Pipeline().Present(cell.target);
    }
}

void ThumbnailPool::Clear() {
    if (cells_.empty())
        return;
    // The cells' targets/scenes may still be referenced by GPU work submitted
    // for the last frame — wait before freeing them.
    if (auto* gfx = svc_.Pipeline().Gfx())
        gfx->WaitIdle();
    for (auto& c : cells_)
        DestroyCell(*c);
    cells_.clear();
    byPath_.clear();
}

} // namespace whiteout::flakes::tools
