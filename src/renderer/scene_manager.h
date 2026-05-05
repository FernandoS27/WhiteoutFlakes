#pragma once

#include "common_types.h"
#include "actor_manager.h"
#include "camera.h"
#include "model_source.h"
#include "model_template_manager.h"
#include "model_types.h"
#include "../io/file_content_provider.h"

#include <atomic>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace WhiteoutDex {

class SceneManager {
public:
    SceneManager()
        : templates_(std::make_unique<ModelTemplateManager>()) {
        activeContentProvider_ = &contentProvider_;
        templates_->SetContentProvider(activeContentProvider_);
    }
    ~SceneManager() = default;
    SceneManager(const SceneManager&)            = delete;
    SceneManager& operator=(const SceneManager&) = delete;

    ActorManager&       Actors()       { return actors_; }
    const ActorManager& Actors() const { return actors_; }
    ActorId             AllocActorId() { return nextActorId_++; }

    ActorId&            NextActorIdRef() { return nextActorId_; }

    ActorId Focus() const             { return focusActor_; }
    void    SetFocus(ActorId id)      { focusActor_ = id; }
    Actor*  FocusActor() const        { return actors_.Find(focusActor_); }

    ActorId&       FocusRef()         { return focusActor_; }
    const ActorId& FocusRef() const   { return focusActor_; }

    ::WhiteoutDex::Camera&       Camera()       { return camera_; }
    const ::WhiteoutDex::Camera& Camera() const { return camera_; }

    void SetCameraPresets(std::vector<CameraPreset> presets) {
        cameraPresets_        = presets;
        pendingCameraPresets_ = std::move(presets);
        cameraDirty_          = true;
        activeCameraPresetIdx_ = -1;
    }
    const std::vector<CameraPreset>& CameraPresets() const { return cameraPresets_; }
    i32   ActiveCameraPresetIdx() const { return activeCameraPresetIdx_; }
    void  SetActiveCameraPresetIdx(i32 idx) { activeCameraPresetIdx_ = idx; }
    bool  CameraLocked() const          { return cameraLocked_; }
    void  SetCameraLocked(bool locked)  { cameraLocked_ = locked; }
    std::optional<std::vector<CameraPreset>> TakePendingCameraPresets() {
        if (!cameraDirty_) return std::nullopt;
        cameraDirty_ = false;
        return std::move(pendingCameraPresets_);
    }

    void SetSequences(std::vector<std::string> names) {
        pendingSequenceNames_ = std::move(names);
        sequencesDirty_ = true;
    }
    void SetSequenceRanges(std::vector<SequenceInfo> ranges) {
        sequenceRanges_ = std::move(ranges);
    }
    const std::vector<SequenceInfo>& SequenceRanges() const { return sequenceRanges_; }
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

    FileContentProvider&       GetContentProvider()       { return contentProvider_; }
    const FileContentProvider& GetContentProvider() const { return contentProvider_; }
    void SetContentProvider(std::shared_ptr<IContentProvider> provider) {
        externalContentProvider_ = std::move(provider);
        activeContentProvider_   = externalContentProvider_
            ? externalContentProvider_.get()
            : static_cast<IContentProvider*>(&contentProvider_);
        templates_->SetContentProvider(activeContentProvider_);
    }
    IContentProvider* ActiveContentProvider() const { return activeContentProvider_; }

    void SetPE1BasePath(const std::filesystem::path& basePath) {
        pe1BasePath_ = basePath;
        contentProvider_.SetBasePath(basePath);
        templates_->SetBasePath(basePath);
    }
    const std::filesystem::path& PE1BasePath() const { return pe1BasePath_; }

    ModelTemplateManager&       Templates()       { return *templates_; }
    const ModelTemplateManager& Templates() const { return *templates_; }

    static constexpr i32 kMaxPE1Depth     = 3;
    static constexpr i32 kMaxPE1Instances = 256;
    i32  PE1InstanceCount() const           { return pe1InstanceCount_; }
    i32& PE1InstanceCountRef()              { return pe1InstanceCount_; }
    void SetPE1InstanceCount(i32 n)         { pe1InstanceCount_ = n; }

private:
    ActorManager actors_;
    ActorId      nextActorId_ = 1;
    ActorId      focusActor_  = 0;

    ::WhiteoutDex::Camera        camera_;
    std::vector<CameraPreset>    cameraPresets_;
    std::vector<CameraPreset>    pendingCameraPresets_;
    bool                         cameraDirty_           = false;
    bool                         cameraLocked_          = false;
    i32                          activeCameraPresetIdx_ = -1;

    std::vector<std::string>     pendingSequenceNames_;
    bool                         sequencesDirty_ = false;
    std::vector<SequenceInfo>    sequenceRanges_;

    std::atomic<i32>             animationTimeMs_{0};

    FileContentProvider                contentProvider_;
    std::shared_ptr<IContentProvider>  externalContentProvider_;
    IContentProvider*                  activeContentProvider_ = nullptr;
    std::filesystem::path              pe1BasePath_;

    std::unique_ptr<ModelTemplateManager> templates_;

    i32 pe1InstanceCount_ = 0;
};

}
