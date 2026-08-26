#include "renderer/animation/animation_driver.h"

namespace whiteout::flakes::renderer::animation {

using namespace ::whiteout::flakes::renderer::model;

void AnimationDriver::Bind(std::shared_ptr<model::IAnimationSource> source) {
    source_ = std::move(source);
    sequences_ = source_ ? source_->GetSequences() : std::vector<SequenceInfo>{};
    playlist_.SetTransitionPolicy(source_ ? source_->DefaultTransition() : TransitionPolicy{});
    // The model's own global loops, restated on every Bind because Bind is
    // both of the moments StarCraft II restates them: building the animation
    // state, and finishing an `.m3a` merge. A host that silenced one has to
    // silence it again afterwards, for the same reason its sequence *indices*
    // are stale — the table it named them in has been rebuilt.
    std::vector<i32> globals;
    for (i32 i = 0; i < (i32)sequences_.size(); ++i)
        if (sequences_[i].alwaysPlays)
            globals.push_back(i);
    playlist_.SetGlobalSequences(std::move(globals));
    // Rebuilt, not appended: Bind is how an actor changes model, and carrying a
    // previous model's solvers forward would aim bone indices at a different
    // skeleton.
    stages_.clear();
    if (source_)
        source_->CreatePoseStages(stages_);
}

void AnimationDriver::Play(std::string_view sequenceName) {
    for (i32 i = 0; i < (i32)sequences_.size(); ++i) {
        if (sequences_[i].name == sequenceName) {
            Play(i);
            return;
        }
    }
}

void AnimationDriver::Advance(i32 nowMs, bool forceLoop) {
    if (!source_ || sequences_.empty())
        return;
    playlist_.Advance(nowMs, sequences_, forceLoop);
    timeMs_ = playlist_.PrimaryTimeMs();
}

std::vector<SequenceInfo> AnimationDriver::Sequences() const {
    return sequences_;
}

FrameState AnimationDriver::Evaluate(const Matrix44f& worldTransform, const Vector3f& cameraPos,
                                     i32 globalTimeMs, const Matrix44f& view) const {
    if (!source_)
        return {};
    // `clip` has to outlive the request — PoseRequest::clips is a view, and
    // OneClip deletes its rvalue overload so a temporary here would not
    // compile rather than dangle.
    const ClipRef clip{.sequence = ActiveSequenceIndex(), .timeMs = timeMs_};
    PoseRequest req = PoseRequest::OneClip(clip);
    req.globalTimeMs = globalTimeMs;
    req.world = worldTransform;
    req.cameraPos = cameraPos;
    req.view = view;
    return source_->Evaluate(req);
}

} // namespace whiteout::flakes::renderer::animation
