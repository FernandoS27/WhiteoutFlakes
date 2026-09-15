#include "renderer/particle/base/particle_curve.h"

#include <algorithm>
#include <cmath>

namespace whiteout::flakes::renderer::particle {

namespace {

// WC3's cell sweep: `repeat` cycles of [start..end] across the segment, with
// the descending case (end < start) inset by one so the range stays
// symmetrical. Preserved verbatim — it is observable in every sprite-sheet
// particle.
i32 ComputeCell(i32 start, i32 end, i32 repeat, f32 t) {
    const f32 r = (repeat < 1) ? 1.0f : static_cast<f32>(repeat);

    const i32 initial = (end >= start) ? start : (start + 1);
    const i32 delta = (end >= start) ? (end - start + 1) : (end - start - 1);

    const f32 effT = (r == 1.0f) ? t : std::fmod(t * r, 1.0f);
    f32 val = static_cast<f32>(initial) + static_cast<f32>(delta) * effT;
    if (val < 0.0f)
        val = 0.0f;
    i32 cell = static_cast<i32>(val);

    // `delta` spans one past the last cell so that the sweep divides evenly, so
    // t == 1 lands on end+1. WC3 never hit that because its sampling bias keeps
    // t below 1 — but a track with no bias (which is what the other formats
    // author) would step off the end of the sprite range. Clamp: a no-op for
    // WC3, correct for everyone else.
    const i32 lo = (std::min)(start, end);
    const i32 hi = (std::max)(start, end);
    if (cell < lo)
        cell = lo;
    if (cell > hi)
        cell = hi;
    return cell;
}

} // namespace

i32 CellAnimTrack::Evaluate(f32 t, u32 hint) const {
    if (segments_.empty())
        return 0;

    usize i = (hint < segments_.size()) ? static_cast<usize>(hint) : 0;
    while (i + 1 < segments_.size() && t > segments_[i].endT)
        ++i;
    while (i > 0 && t <= segments_[i - 1].endT)
        --i;

    const f32 begin = (i == 0) ? 0.0f : segments_[i - 1].endT;
    const f32 span = segments_[i].endT - begin;
    const f32 raw = (span > 0.0f) ? (t - begin) / span : 0.0f;
    const f32 s = ApplyBias(raw, bias_);

    const CellSegment& seg = segments_[i];
    return ComputeCell(seg.start, seg.end, seg.repeat, s);
}

} // namespace whiteout::flakes::renderer::particle
