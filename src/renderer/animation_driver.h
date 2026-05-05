#pragma once

#include "common_types.h"
#include "model_source.h"
#include "types.h"

#include <memory>
#include <string_view>
#include <vector>

namespace WhiteoutDex {

class AnimationDriver {
public:

    void Bind(std::shared_ptr<IAnimationSource> source) { source_ = std::move(source); }
    bool HasSource() const { return static_cast<bool>(source_); }
    const std::shared_ptr<IAnimationSource>& Source() const { return source_; }

    void Play(i32 sequenceIdx, i32 startTimeMs = 0) {
        currentSequenceIdx_ = sequenceIdx;
        timeMs_             = startTimeMs;
    }
    void Play(std::string_view sequenceName);

    i32  ActiveSequenceIndex() const          { return currentSequenceIdx_; }
    void SetActiveSequenceIndex(i32 idx)      { currentSequenceIdx_ = idx; }

    i32  TimeMs() const                       { return timeMs_; }
    void SetTimeMs(i32 ms)                    { timeMs_ = ms; }

    i32  BirthTimeMs() const                  { return birthTimeMs_; }
    void SetBirthTimeMs(i32 ms)               { birthTimeMs_ = ms; }

    std::vector<SequenceInfo> Sequences() const;

    FrameState Evaluate(const Matrix44f& worldTransform,
                        const Vector3f&  cameraPos,
                        i32              globalTimeMs) const;

private:
    std::shared_ptr<IAnimationSource> source_;
    i32 currentSequenceIdx_ = 0;
    i32 timeMs_             = 0;
    i32 birthTimeMs_        = 0;

};

}
