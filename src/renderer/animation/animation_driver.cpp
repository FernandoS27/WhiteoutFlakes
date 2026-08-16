#include "renderer/animation/animation_driver.h"

namespace whiteout::flakes::renderer::animation {

using namespace ::whiteout::flakes::renderer::model;

void AnimationDriver::Bind(std::shared_ptr<model::IAnimationSource> source) {
    source_ = std::move(source);
    sequences_ = source_ ? source_->GetSequences() : std::vector<SequenceInfo>{};
    playlist_.SetTransitionPolicy(source_ ? source_->DefaultTransition() : TransitionPolicy{});
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
                                     i32 globalTimeMs) const {
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
    return source_->Evaluate(req);
}

} // namespace whiteout::flakes::renderer::animation
