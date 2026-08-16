#pragma once

/// @file event_crossing.h
/// @brief Which discrete event keys a frame crossed.
///
/// Extracted from `EventEmitterPool`'s anonymous namespace rather than
/// reimplemented for `.m3` (design B13, plan P-A6). The arithmetic is identical
/// across all three games — every key with `prev < t <= now`, and a loop wrap
/// emits the tail of the window then its head — so a second copy would be a
/// second thing to keep correct. What differs per format is only *which* track
/// a config's times come from and how the window is chosen, and both of those
/// are the caller's business.
///
/// It lives here, not in `m3_animation.h` as the plan sketched, for that
/// reason: nothing about it is StarCraft II specific, and putting the shared
/// definition inside one format's file is how the duplicate would start.
///
/// Header-only so the tests can reach it without linking the pool, which drags
/// in the splat service, the SPN spawner and the sound emitter.

#include "whiteout/flakes/types.h"

#include <algorithm>
#include <vector>

namespace whiteout::flakes::renderer::effects {

/// @brief How many of @p times fall in the half-open interval `(lo, hi]`,
///        clamped to the sequence window `[windowLo, windowHi]`.
///
/// Half-open on purpose: a key exactly at `now` fires this frame, and the same
/// key sitting at the next frame's `prev` does not fire again. That is what
/// stops a stationary cursor re-firing every frame.
inline i32 KeysInHalfOpen(const std::vector<u32>& times, i32 lo, i32 hi, i32 windowLo,
                          i32 windowHi) {
    if (times.empty() || lo >= hi)
        return 0;
    const i32 loB = std::max(lo, windowLo - 1);
    const i32 hiB = std::min(hi, windowHi);
    if (loB >= hiB)
        return 0;
    i32 n = 0;
    for (u32 raw : times) {
        const i32 t = static_cast<i32>(raw);
        if (t > loB && t <= hiB)
            ++n;
    }
    return n;
}

/// @brief Keys crossed going from @p prevFrame to @p nowFrame, wrap included.
///
/// @param prevFrame Last frame's cursor. Seed it with `windowLo - 1` on the
///        first tick and after a sequence switch, so a key sitting exactly on
///        the window's first millisecond still fires — the "skip the first
///        tick" shape swallowed those silently.
///
/// A cursor that went backwards is a loop: the tail `(prev, windowHi]` and the
/// head `(windowLo-1, now]` are both crossed, in that order. Counting them as
/// one interval would report zero, which is how a looping animation's events
/// stop firing after its first cycle.
inline i32 CrossedKeyCount(const std::vector<u32>& times, i32 prevFrame, i32 nowFrame,
                           i32 windowLo, i32 windowHi) {
    if (nowFrame >= prevFrame)
        return KeysInHalfOpen(times, prevFrame, nowFrame, windowLo, windowHi);
    return KeysInHalfOpen(times, prevFrame, windowHi, windowLo, windowHi) +
           KeysInHalfOpen(times, windowLo - 1, nowFrame, windowLo, windowHi);
}

} // namespace whiteout::flakes::renderer::effects
