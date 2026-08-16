#pragma once

#include "clip_playlist.h"
#include "types.h"
#include "whiteout/flakes/pose_stage.h"
#include "whiteout/flakes/model_source.h"
#include "whiteout/flakes/types.h"

#include <memory>
#include <string_view>
#include <vector>

namespace whiteout::flakes::renderer::animation {

class AnimationDriver {
public:
    void Bind(std::shared_ptr<model::IAnimationSource> source);
    bool HasSource() const {
        return static_cast<bool>(source_);
    }
    const std::shared_ptr<model::IAnimationSource>& Source() const {
        return source_;
    }

    void Play(i32 sequenceIdx, i32 startTimeMs = 0) {
        playlist_.SetActiveSequence(sequenceIdx);
        timeMs_ = startTimeMs;
    }
    void Play(std::string_view sequenceName);

    i32 ActiveSequenceIndex() const {
        return playlist_.ActiveSequenceIndex();
    }
    void SetActiveSequenceIndex(i32 idx) {
        playlist_.SetActiveSequence(idx);
    }

    i32 TimeMs() const {
        return timeMs_;
    }
    void SetTimeMs(i32 ms) {
        timeMs_ = ms;
    }

    i32 BirthTimeMs() const {
        return birthTimeMs_;
    }
    void SetBirthTimeMs(i32 ms) {
        birthTimeMs_ = ms;
    }

    /// @brief The playback stack. Layered plays, blend envelopes and the
    ///        scrub/cycle bookkeeping all live here.
    ClipPlaylist& Playlist() {
        return playlist_;
    }
    const ClipPlaylist& Playlist() const {
        return playlist_;
    }

    /// @brief This actor's post-sampling corrections, in run order.
    ///
    /// Built once by @ref Bind, because they are per-actor state (a
    /// rate-limited IK goal, a turret's slew cursor) rather than per-model
    /// data. Empty for wc3, wow, and any `.m3` without solver chunks.
    PoseStageList& PoseStages() {
        return stages_;
    }
    const PoseStageList& PoseStages() const {
        return stages_;
    }

    /// @brief Step the playback stack and latch the primary play's time.
    ///        @p nowMs is the actor clock.
    void Advance(i32 nowMs, bool forceLoop);

    std::vector<model::SequenceInfo> Sequences() const;

    model::FrameState Evaluate(const Matrix44f& worldTransform, const Vector3f& cameraPos,
                               i32 globalTimeMs) const;

private:
    std::shared_ptr<model::IAnimationSource> source_;
    ClipPlaylist playlist_;
    PoseStageList stages_;
    // Cached sequence table. Advance runs every frame for every actor and the
    // source builds this vector (strings included) on each call.
    std::vector<model::SequenceInfo> sequences_;
    i32 timeMs_ = 0;
    i32 birthTimeMs_ = 0;
};

} // namespace whiteout::flakes::renderer::animation
