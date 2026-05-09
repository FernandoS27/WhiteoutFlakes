#pragma once

#include "common_types.h"
#include "model/actor_manager.h"
#include "camera.h"
#include "model/model_source.h"
#include "model/model_template_manager.h"
#include "model/model_types.h"
#include "../io/file_content_provider.h"

#include <atomic>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace whiteout::flakes::renderer {

class SceneManager {
public:
    SceneManager()
        : templates_(std::make_unique<model::ModelTemplateManager>()) {
        activeContentProvider_ = &contentProvider_;
        templates_->SetContentProvider(activeContentProvider_);
    }
    ~SceneManager() = default;
    SceneManager(const SceneManager&)            = delete;
    SceneManager& operator=(const SceneManager&) = delete;

    model::ActorManager&       Actors()       { return actors_; }
    const model::ActorManager& Actors() const { return actors_; }
    model::ActorId             AllocActorId() { return nextActorId_++; }

    model::ActorId&            NextActorIdRef() { return nextActorId_; }

    model::ActorId Focus() const             { return focusActor_; }
    void    SetFocus(model::ActorId id)      { focusActor_ = id; }
    model::Actor*  FocusActor() const        { return actors_.Find(focusActor_); }

    model::ActorId&       FocusRef()         { return focusActor_; }
    const model::ActorId& FocusRef() const   { return focusActor_; }

    ::whiteout::flakes::renderer::Camera&       Camera()       { return camera_; }
    const ::whiteout::flakes::renderer::Camera& Camera() const { return camera_; }

    void SetCameraPresets(std::vector<model::CameraPreset> presets) {
        cameraPresets_        = presets;
        pendingCameraPresets_ = std::move(presets);
        cameraDirty_          = true;
        activeCameraPresetIdx_ = -1;
    }
    const std::vector<model::CameraPreset>& CameraPresets() const { return cameraPresets_; }
    i32   ActiveCameraPresetIdx() const { return activeCameraPresetIdx_; }
    void  SetActiveCameraPresetIdx(i32 idx) { activeCameraPresetIdx_ = idx; }
    bool  CameraLocked() const          { return cameraLocked_; }
    void  SetCameraLocked(bool locked)  { cameraLocked_ = locked; }
    std::optional<std::vector<model::CameraPreset>> TakePendingCameraPresets() {
        if (!cameraDirty_) return std::nullopt;
        cameraDirty_ = false;
        return std::move(pendingCameraPresets_);
    }

    void SetSequences(std::vector<std::string> names) {
        pendingSequenceNames_ = std::move(names);
        sequencesDirty_ = true;
    }
    void SetSequenceRanges(std::vector<model::SequenceInfo> ranges) {
        sequenceRanges_ = std::move(ranges);
    }
    const std::vector<model::SequenceInfo>& SequenceRanges() const { return sequenceRanges_; }
    std::optional<std::vector<std::string>> TakePendingSequences() {
        if (!sequencesDirty_) return std::nullopt;
        sequencesDirty_ = false;
        auto out = std::move(pendingSequenceNames_);
        pendingSequenceNames_.clear();
        return out;
    }

    void SetAnimationTime(i32 ms)      { animationTimeMs_ = ms; }
    i32  GetAnimationTime() const      { return animationTimeMs_.load(); }
    std::atomic<i32>& AnimationTimeAtomic() { return animationTimeMs_; }

    void Update(f32 dtSec);

    // Switch the camera to one of the registered presets, or pass -1 to
    // reset to the default orbital pose. Reads the focus actor's animation
    // index when the preset has an animator callback so live-camera tracks
    // can sample the actor's current sequence.
    void ActivateCameraPreset(i32 idx);

    io::FileContentProvider&       GetContentProvider()       { return contentProvider_; }
    const io::FileContentProvider& GetContentProvider() const { return contentProvider_; }
    void SetContentProvider(std::shared_ptr<io::IContentProvider> provider) {
        externalContentProvider_ = std::move(provider);
        activeContentProvider_   = externalContentProvider_
            ? externalContentProvider_.get()
            : static_cast<io::IContentProvider*>(&contentProvider_);
        templates_->SetContentProvider(activeContentProvider_);
    }
    io::IContentProvider* ActiveContentProvider() const { return activeContentProvider_; }

    void SetPE1BasePath(const std::filesystem::path& basePath) {
        pe1BasePath_ = basePath;
        contentProvider_.SetBasePath(basePath);
        templates_->SetBasePath(basePath);
    }
    const std::filesystem::path& PE1BasePath() const { return pe1BasePath_; }

    model::ModelTemplateManager&       Templates()       { return *templates_; }
    const model::ModelTemplateManager& Templates() const { return *templates_; }

    static constexpr i32 kMaxPE1Depth     = 3;
    static constexpr i32 kMaxPE1Instances = 256;
    i32  PE1InstanceCount() const           { return pe1InstanceCount_; }
    i32& PE1InstanceCountRef()              { return pe1InstanceCount_; }
    void SetPE1InstanceCount(i32 n)         { pe1InstanceCount_ = n; }

    // Scene-wide animation policy: when true, non-looping sequences hold their
    // last frame instead of restarting. Newly-spawned actors inherit it; the
    // setter fans the flag out to existing actors.
    bool IgnoreNonLooping() const { return ignoreNonLooping_; }
    void SetIgnoreNonLooping(bool on) {
        ignoreNonLooping_ = on;
        for (auto& [h, mi] : actors_.All()) {
            if (mi->isPE1Child) continue;
            mi->ignoreNonLooping = on;
        }
    }

private:
    model::ActorManager actors_;
    model::ActorId      nextActorId_ = 1;
    model::ActorId      focusActor_  = 0;

    ::whiteout::flakes::renderer::Camera camera_;
    std::vector<model::CameraPreset>     cameraPresets_;
    std::vector<model::CameraPreset>     pendingCameraPresets_;
    bool                                 cameraDirty_           = false;
    bool                                 cameraLocked_          = false;
    i32                                  activeCameraPresetIdx_ = -1;

    std::vector<std::string>             pendingSequenceNames_;
    bool                                 sequencesDirty_ = false;
    std::vector<model::SequenceInfo>     sequenceRanges_;

    std::atomic<i32>                     animationTimeMs_{0};

    io::FileContentProvider                contentProvider_;
    std::shared_ptr<io::IContentProvider>  externalContentProvider_;
    io::IContentProvider*                  activeContentProvider_ = nullptr;
    std::filesystem::path                  pe1BasePath_;

    std::unique_ptr<model::ModelTemplateManager> templates_;

    i32  pe1InstanceCount_  = 0;
    bool ignoreNonLooping_  = true;
};

}
