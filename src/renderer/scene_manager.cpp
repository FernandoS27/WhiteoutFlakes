#include "renderer/scene_manager.h"

#include "renderer/model_instance.h"
#include "renderer/model_source.h"

#include <algorithm>

namespace WhiteoutDex {

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
