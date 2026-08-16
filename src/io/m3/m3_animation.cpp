#include "io/m3/m3_animation.h"

#include <algorithm>
#include <cmath>

namespace whiteout::flakes::io {

namespace {

using ::whiteout::i32;
using ::whiteout::u16;
using ::whiteout::u32;

// The animRefs word: high half names the typed array, low half the block in
// it. Corpus-asserted rather than assumed — see m3_animation_test.
constexpr M3SdSlot SlotOfWord(u32 w) {
    const u32 hi = w >> 16;
    return hi <= 12u ? static_cast<M3SdSlot>(hi) : M3SdSlot::None;
}
constexpr u32 IndexOfWord(u32 w) {
    return w & 0xFFFFu;
}

// Bounds-check a resolved handle against the container that owns it, so a
// malformed chunk degrades to "no track" instead of an out-of-range read.
template <typename T>
bool InRange(const std::vector<::whiteout::m3::AnimBlock<T>>& arr, u32 i) {
    return i < arr.size();
}

bool HandleValidFor(const ::whiteout::m3::SubTrackContainer& stc, M3TrackHandle h) {
    switch (h.slot) {
    case M3SdSlot::Event:
        return InRange(stc.sdev, h.block);
    case M3SdSlot::Vec2:
        return InRange(stc.sd2v, h.block);
    case M3SdSlot::Vec3:
        return InRange(stc.sd3v, h.block);
    case M3SdSlot::Quat:
        return InRange(stc.sd4q, h.block);
    case M3SdSlot::Color:
        return InRange(stc.sdcc, h.block);
    case M3SdSlot::Float:
        return InRange(stc.sdr3, h.block);
    case M3SdSlot::U8:
        return InRange(stc.sdu8, h.block);
    case M3SdSlot::S16:
        return InRange(stc.sds6, h.block);
    case M3SdSlot::U16:
        return InRange(stc.sdu6, h.block);
    case M3SdSlot::S32:
        return InRange(stc.sds3, h.block);
    case M3SdSlot::U32:
        return InRange(stc.sdu3, h.block);
    case M3SdSlot::Flag:
        return InRange(stc.sdfg, h.block);
    case M3SdSlot::Bounds:
        return InRange(stc.sdmb, h.block);
    default:
        return false;
    }
}

std::span<const i32> TimesOf(const ::whiteout::m3::SubTrackContainer& stc, M3TrackHandle h) {
    switch (h.slot) {
    case M3SdSlot::Vec2:
        return stc.sd2v[h.block].timestamps;
    case M3SdSlot::Vec3:
        return stc.sd3v[h.block].timestamps;
    case M3SdSlot::Quat:
        return stc.sd4q[h.block].timestamps;
    case M3SdSlot::Color:
        return stc.sdcc[h.block].timestamps;
    case M3SdSlot::Float:
        return stc.sdr3[h.block].timestamps;
    case M3SdSlot::U8:
        return stc.sdu8[h.block].timestamps;
    case M3SdSlot::S16:
        return stc.sds6[h.block].timestamps;
    case M3SdSlot::U16:
        return stc.sdu6[h.block].timestamps;
    case M3SdSlot::S32:
        return stc.sds3[h.block].timestamps;
    case M3SdSlot::U32:
        return stc.sdu3[h.block].timestamps;
    case M3SdSlot::Flag:
        return stc.sdfg[h.block].timestamps;
    case M3SdSlot::Bounds:
        return stc.sdmb[h.block].timestamps;
    case M3SdSlot::Event:
        return stc.sdev[h.block].timestamps;
    default:
        return {};
    }
}

} // namespace

void M3AnimTables::Build(const ::whiteout::m3::Model& model) {
    rowOf_.clear();
    table_.clear();
    seqLayers_.clear();

    const std::size_t stcCount = model.subTrackCollections.size();
    stcCount_ = static_cast<u16>((std::min<std::size_t>)(stcCount, 0xFFFFu));

    // Pass 1: every animId any container drives gets a dense row.
    for (const auto& stc : model.subTrackCollections)
        for (u32 id : stc.animIds)
            if (id != 0)
                rowOf_.emplace(id, static_cast<i32>(rowOf_.size()));

    if (rowOf_.empty() || stcCount_ == 0)
        return;

    // Pass 2: fill the (row × container) grid.
    table_.assign(rowOf_.size() * stcCount_, M3TrackHandle{});
    for (u16 s = 0; s < stcCount_; ++s) {
        const auto& stc = model.subTrackCollections[s];
        const std::size_t n = (std::min)(stc.animIds.size(), stc.animRefs.size());
        for (std::size_t k = 0; k < n; ++k) {
            const u32 id = stc.animIds[k];
            if (id == 0)
                continue;
            const auto it = rowOf_.find(id);
            if (it == rowOf_.end())
                continue;
            M3TrackHandle h;
            h.slot = SlotOfWord(stc.animRefs[k]);
            h.block = IndexOfWord(stc.animRefs[k]);
            if (!h.Valid() || !HandleValidFor(stc, h))
                continue;
            table_[static_cast<std::size_t>(it->second) * stcCount_ + s] = h;
        }
    }

    // Pass 3: per sequence, the containers that drive it, priority-desc.
    //
    // The group array is parallel to the sequence array — group i names the
    // containers for sequence i.
    seqLayers_.resize(model.sequences.size());
    for (std::size_t q = 0; q < model.sequences.size(); ++q) {
        if (q >= model.animationGroups.size())
            break;
        auto& out = seqLayers_[q];
        for (u32 idx : model.animationGroups[q].subtrackIndices) {
            if (idx >= stcCount_)
                continue;
            const auto& stc = model.subTrackCollections[idx];
            LayerDef d;
            d.stc = static_cast<u16>(idx);
            d.priority = stc.animPriority;
            d.transparent = stc.runsConcurrent != 0;
            out.push_back(d);
        }
        // Stable, so containers of equal priority keep their authored order.
        std::stable_sort(out.begin(), out.end(),
                         [](const LayerDef& a, const LayerDef& b) { return a.priority > b.priority; });
    }
}

i32 M3AnimTables::DurationOf(const ::whiteout::m3::Model& model, u16 stc, M3TrackHandle h) const {
    if (!h.Valid() || stc >= model.subTrackCollections.size())
        return 0;
    const std::span<const i32> times = TimesOf(model.subTrackCollections[stc], h);
    return times.empty() ? 0 : times.back();
}

M3KeySpan M3LocateKey(std::span<const i32> times, i32 timeMs, bool loop, bool interpolate) {
    M3KeySpan span;
    if (times.empty())
        return span;
    span.valid = true;

    if (times.size() == 1) {
        span.i0 = span.i1 = 0;
        return span;
    }

    // A looping track wraps on its own last key, which is why the caller hands
    // in unwrapped time rather than the sequence-windowed value.
    if (loop) {
        const i32 period = times.back();
        if (period > 0) {
            timeMs %= period;
            if (timeMs < 0)
                timeMs += period;
        }
    }

    if (timeMs <= times.front()) {
        span.i0 = span.i1 = 0;
        return span;
    }
    if (timeMs >= times.back()) {
        span.i0 = span.i1 = times.size() - 1;
        return span;
    }

    const auto it = std::upper_bound(times.begin(), times.end(), timeMs);
    const std::size_t hi = static_cast<std::size_t>(it - times.begin());
    span.i0 = hi - 1;
    span.i1 = hi;

    if (!interpolate) {
        span.i1 = span.i0; // step: hold the left key
        return span;
    }
    const i32 t0 = times[span.i0];
    const i32 t1 = times[span.i1];
    const i32 dt = t1 - t0;
    span.frac = dt > 0 ? static_cast<::whiteout::f32>(timeMs - t0) / static_cast<::whiteout::f32>(dt)
                       : 0.0f;
    return span;
}

::whiteout::Quaternion M3LerpQuatRaw(const ::whiteout::Quaternion& a,
                                     const ::whiteout::Quaternion& b, ::whiteout::f32 t) {
    // No normalise, no sign fix — see the header. Matching this exactly matters
    // on tracks whose adjacent keys are more than 90° apart, where a slerp and
    // this lerp disagree visibly.
    return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t,
            a.w + (b.w - a.w) * t};
}

::whiteout::Quaternion M3SlerpQuat(const ::whiteout::Quaternion& a, const ::whiteout::Quaternion& b,
                                   ::whiteout::f32 t) {
    ::whiteout::f32 dot = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
    ::whiteout::Quaternion e = b;
    if (dot < 0.0f) {
        dot = -dot;
        e = {-b.x, -b.y, -b.z, -b.w};
    }
    if (dot > 0.9995f)
        return M3LerpQuatRaw(a, e, t);

    const ::whiteout::f32 theta = std::acos(dot);
    const ::whiteout::f32 sinTheta = std::sin(theta);
    if (sinTheta <= 1e-6f)
        return M3LerpQuatRaw(a, e, t);
    const ::whiteout::f32 wa = std::sin((1.0f - t) * theta) / sinTheta;
    const ::whiteout::f32 wb = std::sin(t * theta) / sinTheta;
    return {a.x * wa + e.x * wb, a.y * wa + e.y * wb, a.z * wa + e.z * wb, a.w * wa + e.w * wb};
}

} // namespace whiteout::flakes::io
