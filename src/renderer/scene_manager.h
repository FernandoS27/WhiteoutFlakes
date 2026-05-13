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
        activeContentProvider_ = &contentProvider_;
        templates_->SetContentProvider(activeContentProvider_);
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
        return camera_;
    }
    const ::whiteout::flakes::renderer::Camera& Camera() const {
        return camera_;
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

    io::FileContentProvider& GetContentProvider() {
        return contentProvider_;
    }
    const io::FileContentProvider& GetContentProvider() const {
        return contentProvider_;
    }
    void SetContentProvider(std::shared_ptr<io::IContentProvider> provider) {
        externalContentProvider_ = std::move(provider);
        activeContentProvider_ = externalContentProvider_
                                     ? externalContentProvider_.get()
                                     : static_cast<io::IContentProvider*>(&contentProvider_);
        templates_->SetContentProvider(activeContentProvider_);
    }
    io::IContentProvider* ActiveContentProvider() const {
        return activeContentProvider_;
    }

    void SetPE1BasePath(const std::filesystem::path& basePath) {
        pe1BasePath_ = basePath;
        contentProvider_.SetBasePath(basePath);
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

    ::whiteout::flakes::renderer::Camera camera_;

    std::atomic<i32> animationTimeMs_{0};

    io::FileContentProvider contentProvider_;
    std::shared_ptr<io::IContentProvider> externalContentProvider_;
    io::IContentProvider* activeContentProvider_ = nullptr;
    std::filesystem::path pe1BasePath_;

    std::unique_ptr<model::ModelTemplateManager> templates_;

    i32 pe1InstanceCount_ = 0;
};

} // namespace whiteout::flakes::renderer
