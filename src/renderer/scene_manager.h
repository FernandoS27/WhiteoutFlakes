#pragma once

#include "../io/file_content_provider.h"
#include "camera.h"
#include "model/actor_manager.h"
#include "model/model_template_manager.h"
#include "whiteout/flakes/model_source.h"
#include "whiteout/flakes/model_types.h"
#include "whiteout/flakes/types.h"

#include <atomic>
#include <filesystem>
#include <memory>
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
