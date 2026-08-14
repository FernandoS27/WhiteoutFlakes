#include "renderer/animation/animation_driver.h"

namespace whiteout::flakes::renderer::animation {

using namespace ::whiteout::flakes::renderer::model;

void AnimationDriver::Play(std::string_view sequenceName) {
    if (!source_)
        return;
    auto seqs = source_->GetSequences();
    for (i32 i = 0; i < (i32)seqs.size(); ++i) {
        if (seqs[i].name == sequenceName) {
            Play(i);
            return;
        }
    }
}

std::vector<SequenceInfo> AnimationDriver::Sequences() const {
    if (!source_)
        return {};
    return source_->GetSequences();
}

FrameState AnimationDriver::Evaluate(const Matrix44f& worldTransform, const Vector3f& cameraPos,
                                     i32 globalTimeMs) const {
    if (!source_)
        return {};
    // `clip` has to outlive the request — PoseRequest::clips is a view, and
    // OneClip deletes its rvalue overload so a temporary here would not
    // compile rather than dangle.
    const ClipRef clip{.sequence = currentSequenceIdx_, .timeMs = timeMs_};
    PoseRequest req = PoseRequest::OneClip(clip);
    req.globalTimeMs = globalTimeMs;
    req.world = worldTransform;
    req.cameraPos = cameraPos;
    return source_->Evaluate(req);
}

} // namespace whiteout::flakes::renderer::animation
