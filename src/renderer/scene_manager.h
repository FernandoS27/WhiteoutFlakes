#pragma once

#include "../io/file_content_provider.h"
#include "camera.h"
#include "model/actor_manager.h"
#include "model/model_template_manager.h"
#include "whiteout/flakes/enums.h" // ProductId
#include "whiteout/flakes/model_source.h"
#include "whiteout/flakes/model_types.h"
#include "whiteout/flakes/types.h"

#include <atomic>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace whiteout::flakes::renderer {

// SceneManager is the renderer's container for runtime scene state. It owns
// the actor map, the model template cache, the content provider, the wall
// clock, and the Camera (which the host mutates via Camera() each frame).
//
// Viewer-unique behavior — focus actor tracking, sequence-name caches, camera
// presets and preset activation, walk-drift, sequence pickers — does NOT live
// here. Hosts (basic_viewer, max_plugin) own those concerns and drive the
// renderer through Loader().SpawnUnit / Actor field mutators / Camera().
class SceneManager {
public:
    SceneManager() : templates_(std::make_unique<model::ModelTemplateManager>()) {
        // The internal FileContentProvider is created LAZILY (see
        // EnsureInternalProvider): constructing it opens the WC3 CASC and spawns
        // a worker-thread pool, which is wasteful for scenes that run on a host-
        // supplied external provider (e.g. the model explorer's per-cell scenes,
        // which all share one provider). Scenes that never touch their own
        // provider never pay that cost.
        // Camera 0 always exists — it's the legacy single camera that Camera()
        // (no-arg) and the RenderFrame shim use. Additional cameras (for extra
        // viewports onto this scene) are appended via AddCamera().
        cameras_.push_back(std::make_unique<::whiteout::flakes::renderer::Camera>());
    }
    ~SceneManager() = default;
    SceneManager(const SceneManager&) = delete;
    SceneManager& operator=(const SceneManager&) = delete;

    model::ActorManager& Actors() {
        return actors_;
    }
    const model::ActorManager& Actors() const {
        return actors_;
    }
    model::ActorId AllocActorId() {
        return nextActorId_++;
    }

    /// @brief Shift the handle sequence before anything spawns. Exists for the
    ///        draw-trace gate's perturbation arm (REFACTOR_PLAN.md, GATE P0):
    ///        recording twice from the same binary proves nothing, because an
    ///        unseeded rand() and a fixed insertion sequence are both
    ///        deterministic within a process. Moving the first handle changes
    ///        every actor's hash bucket, which is what actually exposes a draw
    ///        path that depends on unordered_map iteration order.
    void SeedActorIds(model::ActorId first) {
        if (first != 0)
            nextActorId_ = first;
    }

    /// @brief Which game's data this scene holds. Selects the render profile,
    ///        the model adapter and the surface table.
    ///
    /// Scene state rather than a global: the model explorer runs many scenes
    /// at once, and there is no reason two of them must show the same game.
    /// `Neutral` is the honest starting value — a scene with nothing in it
    /// belongs to no product.
    ProductId Product() const {
        return product_;
    }
    void SetProduct(ProductId p) {
        product_ = p;
        // The scene's product is also which storage rules its content obeys —
        // WoW wants fileDataIDs and a listfile, StarCraft II wants CASC only
        // and may need Heroes' install open alongside it. Forwarding here is
        // what stops "the scene is WoW" and "the provider is reading WC3" from
        // drifting apart.
        //
        // Only when the internal provider already exists: creating it opens a
        // CASC and spawns threads, which is exactly the cost SetContentProvider
        // goes out of its way not to pay on an externally-provided scene.
        if (contentProvider_ && !externalContentProvider_)
            contentProvider_->SetGame(p);
    }

    /// @brief This scene's shading mode (classic SD vs HD Reforged), scene
    ///        state for the same reason Product() is: the model explorer runs
    ///        many scenes at once, each in its model's own mode. Unset means
    ///        "follow the global RenderSettings mode" — the honest state for a
    ///        scene nothing has loaded into, and what keeps hosts that only
    ///        ever set the global (gates, bindings) behaving as before. The
    ///        loader settles it from the parsed template before anything
    ///        latches; RenderService::EffectiveRenderMode resolves the
    ///        fallback.
    std::optional<::whiteout::flakes::RenderMode> RenderModeOverride() const {
        return renderMode_;
    }
    void SetRenderMode(::whiteout::flakes::RenderMode m) {
        renderMode_ = m;
        // Forward the HD texture overlay. Unlike SetProduct's internal-only
        // forward, this reaches shared external providers too: the overlay is
        // a per-read layer preference that loads flip mid-session by design,
        // and RenderService::SetActiveScene re-imposes it per activation, so
        // sibling scenes on the same provider get their own state back the
        // moment they run.
        if (auto* p = ActiveContentProviderIfAny())
            p->SetHdMode(m == ::whiteout::flakes::RenderMode::HD);
    }

    /// @brief Scene-level override of the SD-through-HDR opt-in
    ///        (RenderSettings::SceneHdrInSd). Unset follows the global flag;
    ///        the thumbnail pool pins its cell scenes true once at creation
    ///        instead of flipping the global around every render.
    std::optional<bool> SceneHdrInSdOverride() const {
        return sceneHdrInSd_;
    }
    void SetSceneHdrInSdOverride(bool on) {
        sceneHdrInSd_ = on;
    }

    ::whiteout::flakes::renderer::Camera& Camera() {
        return *cameras_[0];
    }
    const ::whiteout::flakes::renderer::Camera& Camera() const {
        return *cameras_[0];
    }

    // Camera set. Index 0 is the default camera; higher indices are extra
    // cameras for additional viewports onto this scene. Out-of-range indices
    // clamp to the default so a stale handle can never dangle.
    ::whiteout::flakes::renderer::Camera& Camera(i32 idx) {
        return (idx >= 0 && idx < (i32)cameras_.size()) ? *cameras_[idx] : *cameras_[0];
    }
    const ::whiteout::flakes::renderer::Camera& Camera(i32 idx) const {
        return (idx >= 0 && idx < (i32)cameras_.size()) ? *cameras_[idx] : *cameras_[0];
    }
    i32 CameraCount() const {
        return (i32)cameras_.size();
    }
    // Append a new camera and return its index. unique_ptr storage keeps
    // existing Camera& references stable across this growth.
    i32 AddCamera() {
        cameras_.push_back(std::make_unique<::whiteout::flakes::renderer::Camera>());
        return (i32)cameras_.size() - 1;
    }

    void SetAnimationTime(i32 ms) {
        animationTimeMs_ = ms;
    }
    i32 GetAnimationTime() const {
        return animationTimeMs_.load();
    }

    // Advances the wall clock and each top-level Unit actor's playback clock
    // by dtSec (scaled per-actor by Actor::playbackSpeed). Hosts that want
    // fully bespoke per-actor scheduling can skip this and call Actor::Advance
    // directly on the actors they care about.
    void Update(f32 dtSec);

    // ---- Transport ---------------------------------------------------
    // The scene owns one playback state, and both halves of a frame read it:
    // Update() for animation clocks, FrameTicker::Tick() for particle, PE1,
    // ribbon and corn-fx simulation. Keeping it here rather than in either
    // caller is what makes a pause apply to models and effects alike, whether
    // the host drives one entry point or both.
    void SetPlaybackState(PlaybackState s) {
        playbackState_ = s;
    }
    PlaybackState GetPlaybackState() const {
        return playbackState_.load();
    }
    bool IsPaused() const {
        return playbackState_.load() != PlaybackState::Playing;
    }

    // Multiplies every advancing dt. Independent of the transport state, so
    // slow-motion survives a pause/resume.
    void SetTimeScale(f32 s) {
        timeScale_ = s;
    }
    f32 GetTimeScale() const {
        return timeScale_.load();
    }

    // The dt a frame should actually advance by. Both halves call this rather
    // than branching on the state themselves, so they cannot disagree.
    f32 EffectiveDt(f32 dtSec) const {
        return IsPaused() ? 0.0f : dtSec * timeScale_.load();
    }

    io::FileContentProvider& GetContentProvider() {
        return EnsureInternalProvider();
    }
    const io::FileContentProvider& GetContentProvider() const {
        return const_cast<SceneManager*>(this)->EnsureInternalProvider();
    }
    void SetContentProvider(std::shared_ptr<io::IContentProvider> provider) {
        externalContentProvider_ = std::move(provider);
        // Point the active provider + template cache at the external one without
        // forcing the (expensive) internal provider into existence.
        activeContentProvider_ =
            externalContentProvider_
                ? externalContentProvider_.get()
                : static_cast<io::IContentProvider*>(contentProvider_.get());
        templates_->SetContentProvider(ActiveContentProvider());
    }
    io::IContentProvider* ActiveContentProvider() const {
        if (externalContentProvider_)
            return externalContentProvider_.get();
        // No external provider — fall back to the internal one, creating it on
        // first use.
        return &const_cast<SceneManager*>(this)->EnsureInternalProvider();
    }
    /// @brief Non-creating peek at the provider this scene reads through.
    ///        Unlike ActiveContentProvider() it never realises the internal
    ///        provider, so state imposition on scene activation stays free for
    ///        scenes that never read. Null when the scene has no provider yet.
    io::IContentProvider* ActiveContentProviderIfAny() const {
        if (externalContentProvider_)
            return externalContentProvider_.get();
        return contentProvider_.get();
    }

    void SetPE1BasePath(const std::filesystem::path& basePath) {
        pe1BasePath_ = basePath;
        // A scene that loads through its OWN provider needs that provider (and
        // its template-cache wiring) in place before SpawnUnit reads anything.
        // SetPE1BasePath is the host's "about to load into this scene" signal,
        // so realise the internal provider here when no external one is set.
        // Scenes on a shared external provider (explorer cells) skip this and
        // never construct an internal provider.
        if (!externalContentProvider_)
            EnsureInternalProvider();
        if (contentProvider_)
            contentProvider_->SetBasePath(basePath);
        templates_->SetBasePath(basePath);
    }
    const std::filesystem::path& PE1BasePath() const {
        return pe1BasePath_;
    }

    model::ModelTemplateManager& Templates() {
        return *templates_;
    }
    const model::ModelTemplateManager& Templates() const {
        return *templates_;
    }

    i32 PE1InstanceCount() const {
        return pe1InstanceCount_;
    }
    void IncrementPE1Instances() {
        ++pe1InstanceCount_;
    }
    void DecrementPE1Instances() {
        if (pe1InstanceCount_ > 0)
            --pe1InstanceCount_;
    }

private:
    model::ActorManager actors_;
    model::ActorId nextActorId_ = 1;
    ProductId product_ = ProductId::Neutral;
    std::optional<::whiteout::flakes::RenderMode> renderMode_;
    std::optional<bool> sceneHdrInSd_;

    std::vector<std::unique_ptr<::whiteout::flakes::renderer::Camera>> cameras_;

    std::atomic<i32> animationTimeMs_{0};
    std::atomic<PlaybackState> playbackState_{PlaybackState::Playing};
    std::atomic<f32> timeScale_{1.0f};

    // Lazily created (see EnsureInternalProvider) so scenes that use a
    // host-supplied external provider never open a CASC or spawn IO threads.
    std::unique_ptr<io::FileContentProvider> contentProvider_;
    std::shared_ptr<io::IContentProvider> externalContentProvider_;
    io::IContentProvider* activeContentProvider_ = nullptr;
    std::filesystem::path pe1BasePath_;

    io::FileContentProvider& EnsureInternalProvider() {
        if (!contentProvider_) {
            contentProvider_ = std::make_unique<io::FileContentProvider>();
            if (!pe1BasePath_.empty())
                contentProvider_->SetBasePath(pe1BasePath_);
            // A scene whose product was set before it first touched its own
            // provider — the ordinary order for a host that picks the game up
            // front — would otherwise get a Warcraft III provider for a WoW
            // scene. SetGame no-ops when the product is already Wc3.
            if (product_ != ProductId::Neutral)
                contentProvider_->SetGame(product_);
            if (!externalContentProvider_) {
                activeContentProvider_ = contentProvider_.get();
                templates_->SetContentProvider(activeContentProvider_);
            }
        }
        return *contentProvider_;
    }

    std::unique_ptr<model::ModelTemplateManager> templates_;

    i32 pe1InstanceCount_ = 0;
};

} // namespace whiteout::flakes::renderer
