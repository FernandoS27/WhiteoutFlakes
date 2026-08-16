#pragma once

// ============================================================================
// M2 animation primitives — track sampling, the compressed quaternion, and the
// client's sequence-name table.
//
// The shape of an `.m2` track is not the shape of an MDX one, so this is a
// separate file rather than a second overload set on mdx_animation.h:
//
//   * A track is an array *of arrays*. `values[i]` is the key list for
//     sequence i, not for the whole model, and a sequence with no keys of its
//     own falls back to sub-array 0 rather than to the previous key.
//   * Rotation keys are four 16-bit words, and the client's decode is
//     `raw * (2/65535) - 1` — not the `(v < 0 ? v + 32768 : v - 32767)/32767`
//     the wiki publishes. The two agree to within one part in 65535 and the
//     binary is what this follows (`M2AnimateTrack<M2CompQuat, C4Quaternion>`).
//   * A track carrying a global-sequence id ignores the clip time entirely and
//     reads a free-running model clock modulo that sequence's period.
//
// Verified against a fully-symbolised WoW 6.0.1 macOS client:
// `M2AnimateTrack<T, U>` @ 0x100f716e0 (C3Vector), 0x100f63f40 (M2CompQuat),
// 0x100f70d90 (fixed16). Function names are the client's own symbols.
// ============================================================================

#include "whiteout/flakes/types.h"

#include <whiteout/models/m2/types.h>

#include <algorithm>
#include <cstddef>
#include <span>
#include <string_view>
#include <vector>

namespace whiteout::flakes::io {

/// @brief Where to sample an `.m2` track.
///
/// `sequence` selects the per-sequence sub-array; `timeMs` is local to that
/// sequence. `globalTimeMs` is separate because a global-sequence track never
/// participates in the clip's timeline — the client reads
/// `model->globalSequenceTimes[id]`, a clock that keeps running whatever the
/// model is playing.
struct M2AnimTime {
    i32 sequence = 0;
    i32 timeMs = 0;
    i32 globalTimeMs = 0;
    std::span<const u32> globalLoops;
};

/// @brief One M2CompQuat word to `[-1, 1]`. See the header note on why this is
///        not the wiki's formula.
inline f32 M2DecodeQuatComponent(u16 raw) {
    return static_cast<f32>(raw) * (2.0f / 65535.0f) - 1.0f;
}

inline Quaternion M2DecodeQuat(const ::whiteout::m2::CompatQuaternion& q) {
    return Quaternion{M2DecodeQuatComponent(q.data[0]), M2DecodeQuatComponent(q.data[1]),
                      M2DecodeQuatComponent(q.data[2]), M2DecodeQuatComponent(q.data[3])};
}

/// @brief fixed16 (texture weights, colour alpha) to `[0, 1]`.
inline f32 M2DecodeFixed16(i16 v) {
    return static_cast<f32>(v) * (1.0f / 32767.0f);
}

/// @brief The key pair a sample falls between, plus the blend factor.
///
/// `valid == false` means the track has nothing for this sequence and the
/// caller must use the animref default — the branch `M2AnimateTrack` takes
/// when `values[seq].count == 0`.
struct M2KeySpan {
    i32 k0 = 0;
    i32 k1 = 0;
    f32 blend = 0.0f;
    i32 sub = 0; ///< Which values sub-array `k0`/`k1` index into.
    bool valid = false;
};

namespace detail {

/// The sub-array a track reads for @p sequence: its own if it has one, else 0.
/// The client clamps identically (`if (seqIdx >= track.count) seqIdx = 0`),
/// which is what makes an aliased sequence fall back to the base clip's keys
/// rather than to no keys at all.
inline usize M2SubArray(usize count, i32 sequence) {
    if (count == 0)
        return 0;
    const usize s = static_cast<usize>(sequence < 0 ? 0 : sequence);
    return s < count ? s : 0;
}

} // namespace detail

/// @brief Locate the keys for @p track at @p at.
///
/// Templated only because `AnimationTrack<T>` carries its values by type; the
/// search itself reads nothing but `timestamps`.
template <class T>
M2KeySpan LocateM2Key(const ::whiteout::m2::AnimationTrack<T>& track, const M2AnimTime& at) {
    M2KeySpan out;

    const bool global = track.globalSequenceId != 0xFFFF;
    const usize vsub = global ? 0 : detail::M2SubArray(track.values.size(), at.sequence);
    if (vsub >= track.values.size() || track.values[vsub].empty())
        return out;
    out.sub = static_cast<i32>(vsub);

    i32 time = at.timeMs;
    if (global) {
        // The model clock modulo the global sequence's period. A zero period
        // means the sequence never advances, which the client expresses by
        // leaving its timer at 0.
        const u32 period =
            (track.globalSequenceId < at.globalLoops.size()) ? at.globalLoops[track.globalSequenceId]
                                                             : 0u;
        const u32 t = static_cast<u32>(at.globalTimeMs > 0 ? at.globalTimeMs : 0);
        time = period ? static_cast<i32>(t % period) : 0;
    }

    const usize tsub = global ? 0 : detail::M2SubArray(track.timestamps.size(), at.sequence);
    const std::vector<u32>* ts =
        (tsub < track.timestamps.size()) ? &track.timestamps[tsub] : nullptr;
    const usize keyCount =
        std::min<usize>(ts ? ts->size() : usize{0}, track.values[vsub].size());

    out.valid = true;
    if (keyCount <= 1)
        return out; // single key, or a values array with no timestamps: hold it

    const u32 t = static_cast<u32>(time < 0 ? 0 : time);
    // Last key at or before `t`. The client reaches the same index by a
    // hint-seeded linear walk that degrades to a binary search; the hint is a
    // per-track cache this evaluator does not keep, and the answer is the same.
    usize k0 = 0;
    {
        const auto begin = ts->begin();
        const auto it = std::upper_bound(begin, begin + static_cast<isize>(keyCount), t);
        k0 = (it == begin) ? 0 : static_cast<usize>((it - begin) - 1);
    }
    const usize k1 = (k0 + 1 < keyCount) ? k0 + 1 : k0;

    out.k0 = static_cast<i32>(k0);
    out.k1 = static_cast<i32>(k1);
    if (k1 != k0) {
        const u32 a = (*ts)[k0], b = (*ts)[k1];
        // Clamped, unlike the client: sampling before the first key makes its
        // linear-walk branch extrapolate backwards while its binary-search
        // branch bails to key 0. Every shipped track starts at 0, so the two
        // never disagree in practice; holding the first key is the reading that
        // is right either way.
        out.blend = (b > a) ? std::clamp(static_cast<f32>(t - a) / static_cast<f32>(b - a), 0.0f,
                                         1.0f)
                            : 0.0f;
    }
    return out;
}

/// @brief `true` when the track steps rather than interpolates.
///
/// Bezier and Hermite step-through to linear here: `M2AnimateTrack` branches on
/// `interpolationType != 0` alone, and the spline forms are reached only
/// through `M2AnimateSplineTrack`, which nothing but cameras uses.
inline bool M2TrackSteps(const ::whiteout::m2::AnimationTrackBase& track) {
    return track.interpolationType == ::whiteout::m2::InterpolationType::None;
}

/// @brief `Vector3f` track — translation, scale, colour.
Vector3f SampleM2Vec3(const ::whiteout::m2::AnimationTrack<Vector3f>& track, const M2AnimTime& at,
                      const Vector3f& def);

/// @brief Rotation track. Keys are nlerped, not slerped: the client's
///        `M2AnimateTrack<M2CompQuat, C4Quaternion>` calls `C4Quaternion::Nlerp`
///        and does not test the dot sign, so neighbouring keys are trusted to
///        share a hemisphere.
Quaternion SampleM2Quat(const ::whiteout::m2::AnimationTrack<::whiteout::m2::CompatQuaternion>& tr,
                        const M2AnimTime& at, const Quaternion& def);

/// @brief The same track, keyed by uncompressed `C4Quaternion`. Texture
///        transforms are stored that way at every M2 version; bone rotations
///        never are.
Quaternion SampleM2Quat(const ::whiteout::m2::AnimationTrack<Quaternion>& tr, const M2AnimTime& at,
                        const Quaternion& def);

/// @brief fixed16 track — texture weights and colour alpha.
f32 SampleM2Fixed16(const ::whiteout::m2::AnimationTrack<i16>& track, const M2AnimTime& at, f32 def);

/// @brief `f32` track — light intensities and attenuation radii.
f32 SampleM2Float(const ::whiteout::m2::AnimationTrack<f32>& track, const M2AnimTime& at, f32 def);

/// @brief One key of a compressed particle-gravity track.
///
/// The four bytes are `{i8 x, i8 y, i16 z}`: a unit direction whose Z is
/// recovered from the other two, and a magnitude in the `i16` whose SIGN picks
/// the Z hemisphere. @p packed is that word wearing a float's clothes — it is
/// not a number and must never be interpolated before decoding.
Vector3f M2DecodeCompressedGravity(f32 packed);

/// @brief The particle gravity track, always as a vector.
///
/// @p compressed says the keys are packed (file flag `CompressedGravity`);
/// otherwise they are a downward magnitude, negated onto -Z. The client
/// decompresses the whole track at load and interpolates decoded vectors, so
/// this decodes both endpoints and blends those.
Vector3f SampleM2ParticleGravity(const ::whiteout::m2::AnimationTrack<f32>& track,
                                 const M2AnimTime& at, bool compressed);

/// @brief `u8` track — light visibility, and nothing else in the format.
///
/// Holds the key rather than interpolating. `M2AnimateTrack<uchar, uchar>` would
/// lerp a linear track, but the arithmetic truncates on the way back to `uchar`,
/// so a 0→1 segment stays 0 until it lands on the far key — which is what
/// holding gives, and what the on/off flag the value feeds actually wants.
u8 SampleM2U8(const ::whiteout::m2::AnimationTrack<u8>& track, const M2AnimTime& at, u8 def);

/// @brief The client's `AnimationData` name for @p animationId, or an empty
///        view when the id is outside the table.
///
/// Dumped from the 6.0.1 client's `s_animationNames`, the array `CGUnit_C`
/// reports animation warnings against. `.m2` sequences are keyed by this id, so
/// without it a sequence list reads as bare numbers.
std::string_view M2AnimationName(u16 animationId);

} // namespace whiteout::flakes::io
