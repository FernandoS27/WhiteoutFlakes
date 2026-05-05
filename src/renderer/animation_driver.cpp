#include "renderer/animation_driver.h"

namespace WhiteoutDex {

void AnimationDriver::Play(std::string_view sequenceName) {
    if (!source_) return;
    auto seqs = source_->GetSequences();
    for (i32 i = 0; i < (i32)seqs.size(); ++i) {
        if (seqs[i].name == sequenceName) {
            Play(i);
            return;
        }
    }

}

std::vector<SequenceInfo> AnimationDriver::Sequences() const {
    if (!source_) return {};
    return source_->GetSequences();
}

FrameState AnimationDriver::Evaluate(const Matrix44f& worldTransform,
                                     const Vector3f&  cameraPos,
                                     i32              globalTimeMs) const {
    if (!source_) return {};
    return source_->Evaluate(currentSequenceIdx_, timeMs_, globalTimeMs,
                             worldTransform, cameraPos);
}

}
