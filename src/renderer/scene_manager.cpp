#include "renderer/scene_manager.h"

#include "renderer/model/model_instance.h"
#include "renderer/model/model_source.h"

#include <algorithm>

namespace whiteout::flakes::renderer {

using namespace ::whiteout::flakes::renderer::model;

void SceneManager::ActivateCameraPreset(i32 idx) {
    if (idx < 0 || idx >= (i32)cameraPresets_.size()) {
        camera_.SetOrbitalMode();
        camera_.SetFovDiagonal(Camera::kDefaultFovDiagonal);
        camera_.SetClip(Camera::kDefaultNearZ, Camera::kDefaultFarZ);
        activeCameraPresetIdx_ = -1;
        return;
    }
    const auto& p = cameraPresets_[idx];
    Vector3f pos  = p.position;
    Vector3f tgt  = p.target;
    f32      roll = p.staticRoll;

    if (p.animator) {
        i32 seqStart = 0, seqEnd = 0;

        Actor* focus  = FocusActor();
        i32    seqIdx = focus ? focus->animation.ActiveSequenceIndex() : 0;
        if (seqIdx >= 0 && seqIdx < (i32)sequenceRanges_.size()) {
            seqStart = sequenceRanges_[seqIdx].startMs;
            seqEnd   = sequenceRanges_[seqIdx].endMs;
        }

        if (seqStart == 0 && seqEnd == 0) seqEnd = 1 << 30;

        const i32 sampleMs = focus ? focus->animation.TimeMs()
                                   : animationTimeMs_.load();
        p.animator(pos, tgt, roll, sampleMs, seqStart, seqEnd);
    }

    camera_.SetDirectPose(pos, tgt, roll);

    const f32 fov = (p.fovDiagonal > 1e-3f) ? p.fovDiagonal
                                            : Camera::kDefaultFovDiagonal;
    camera_.SetFovDiagonal(fov);
    camera_.SetClip(p.zNear, p.zFar);
    activeCameraPresetIdx_ = idx;
}

void SceneManager::Update(f32 dtSec) {
    const i32 dtMs = (dtSec > 0.0f) ? (i32)(dtSec * 1000.0f + 0.5f) : 0;
    if (dtMs > 0) animationTimeMs_.fetch_add(dtMs);
    const i32 now = animationTimeMs_.load();

    for (auto& [h, mi] : actors_.All()) {

        if (mi->isPE1Child) continue;

        if (mi->externallyDriven) continue;
        if (!mi->animation.HasSource()) continue;

        const auto seqs = mi->animation.Sequences();
        if (seqs.empty()) continue;

        const i32 rawIdx     = mi->animation.ActiveSequenceIndex();
        const i32 boundedIdx = ((rawIdx % (i32)seqs.size()) + (i32)seqs.size()) % (i32)seqs.size();
        if (rawIdx != mi->prevActiveSequence) {
            mi->sequenceStartTimeMs = now;
            mi->prevActiveSequence  = rawIdx;
        }

        const auto& seq      = seqs[boundedIdx];
        const i32   duration = seq.endMs - seq.startMs;
        i32 elapsed = now - mi->sequenceStartTimeMs;
        if (elapsed < 0) elapsed = 0;

        i32 frameMs;
        if (duration <= 0) {
            frameMs = seq.startMs;
        } else if (seq.nonLooping && !mi->ignoreNonLooping) {
            frameMs = seq.startMs + (std::min)(elapsed, duration);
        } else {
            frameMs = seq.startMs + (elapsed % duration);
        }
        mi->animation.SetTimeMs(frameMs);
    }
}

}
