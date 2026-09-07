#include "io/m2/m2_animation.h"

#include <cmath>
#include <cstring>

#include <whiteout/models/m2/animation_names.h>

namespace whiteout::flakes::io {

namespace wm2 = ::whiteout::m2;

Vector3f SampleM2Vec3(const wm2::AnimationTrack<Vector3f>& track, const M2AnimTime& at,
                      const Vector3f& def) {
    const M2KeySpan k = LocateM2Key(track, at);
    if (!k.valid)
        return def;
    const auto& v = track.values[static_cast<usize>(k.sub)];
    const Vector3f& a = v[static_cast<usize>(k.k0)];
    if (k.k0 == k.k1 || M2TrackSteps(track))
        return a;
    const Vector3f& b = v[static_cast<usize>(k.k1)];
    return {a.x + (b.x - a.x) * k.blend, a.y + (b.y - a.y) * k.blend,
            a.z + (b.z - a.z) * k.blend};
}

namespace {

// The two key encodings a rotation track can carry. Which one a track uses is
// a property of what it drives, not of the file version.
Quaternion M2QuatKey(const wm2::CompatQuaternion& q) {
    return M2DecodeQuat(q);
}
Quaternion M2QuatKey(const Quaternion& q) {
    return q;
}

template <class Key>
Quaternion SampleQuatTrack(const wm2::AnimationTrack<Key>& track, const M2AnimTime& at,
                           const Quaternion& def) {
    const M2KeySpan k = LocateM2Key(track, at);
    if (!k.valid)
        return def;
    const auto& v = track.values[static_cast<usize>(k.sub)];
    const Quaternion a = M2QuatKey(v[static_cast<usize>(k.k0)]);
    if (k.k0 == k.k1 || M2TrackSteps(track))
        return a;
    const Quaternion b = M2QuatKey(v[static_cast<usize>(k.k1)]);

    // Nlerp, and no shortest-path sign flip: C4Quaternion::Nlerp lerps the four
    // components and renormalises, full stop. Flipping here would be a
    // *different* rotation than the client produces wherever a key pair
    // straddles hemispheres.
    Quaternion q{a.x + (b.x - a.x) * k.blend, a.y + (b.y - a.y) * k.blend,
                 a.z + (b.z - a.z) * k.blend, a.w + (b.w - a.w) * k.blend};
    const f32 len2 = q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w;
    if (len2 > 1e-12f) {
        const f32 inv = 1.0f / std::sqrt(len2);
        q.x *= inv;
        q.y *= inv;
        q.z *= inv;
        q.w *= inv;
    }
    return q;
}

} // namespace

Quaternion SampleM2Quat(const wm2::AnimationTrack<wm2::CompatQuaternion>& track,
                        const M2AnimTime& at, const Quaternion& def) {
    return SampleQuatTrack(track, at, def);
}

Quaternion SampleM2Quat(const wm2::AnimationTrack<Quaternion>& track, const M2AnimTime& at,
                        const Quaternion& def) {
    return SampleQuatTrack(track, at, def);
}

f32 SampleM2Fixed16(const wm2::AnimationTrack<i16>& track, const M2AnimTime& at, f32 def) {
    const M2KeySpan k = LocateM2Key(track, at);
    if (!k.valid)
        return def;
    const auto& v = track.values[static_cast<usize>(k.sub)];
    const f32 a = M2DecodeFixed16(v[static_cast<usize>(k.k0)]);
    if (k.k0 == k.k1 || M2TrackSteps(track))
        return a;
    return a + (M2DecodeFixed16(v[static_cast<usize>(k.k1)]) - a) * k.blend;
}

f32 SampleM2Float(const wm2::AnimationTrack<f32>& track, const M2AnimTime& at, f32 def) {
    const M2KeySpan k = LocateM2Key(track, at);
    if (!k.valid)
        return def;
    const auto& v = track.values[static_cast<usize>(k.sub)];
    const f32 a = v[static_cast<usize>(k.k0)];
    if (k.k0 == k.k1 || M2TrackSteps(track))
        return a;
    return a + (v[static_cast<usize>(k.k1)] - a) * k.blend;
}

Vector3f M2DecodeCompressedGravity(f32 packed) {
    // Bit-for-bit as CM2Shared::DecompressParticleSequence decodes it,
    // arithmetic order included — the client folds `1 - dx*dx` before
    // subtracting `dy*dy`, and takes the Z hemisphere from the magnitude's sign
    // rather than storing it in the direction.
    u8 raw[4];
    std::memcpy(raw, &packed, 4);
    const f32 dx = static_cast<f32>(static_cast<i8>(raw[0])) * 0.0078125f;
    const f32 dy = static_cast<f32>(static_cast<i8>(raw[1])) * 0.0078125f;
    i16 mz = 0;
    std::memcpy(&mz, raw + 2, 2);
    f32 mag = static_cast<f32>(mz) * 0.042385526f;

    const f32 zz = (1.0f - dx * dx) - dy * dy;
    f32 dz = std::sqrt(zz > 0.0f ? zz : 0.0f);
    if (mag < 0.0f) {
        dz = -dz;
        mag = -mag;
    }
    return {dx * mag, dy * mag, dz * mag};
}

Vector3f SampleM2ParticleGravity(const wm2::AnimationTrack<f32>& track, const M2AnimTime& at,
                                 bool compressed) {
    if (!compressed)
        return {0.0f, 0.0f, -SampleM2Float(track, at, 0.0f)};

    const M2KeySpan k = LocateM2Key(track, at);
    if (!k.valid)
        return {0.0f, 0.0f, 0.0f};
    const auto& v = track.values[static_cast<usize>(k.sub)];
    const Vector3f a = M2DecodeCompressedGravity(v[static_cast<usize>(k.k0)]);
    if (k.k0 == k.k1 || M2TrackSteps(track))
        return a;
    const Vector3f b = M2DecodeCompressedGravity(v[static_cast<usize>(k.k1)]);
    return {a.x + (b.x - a.x) * k.blend, a.y + (b.y - a.y) * k.blend,
            a.z + (b.z - a.z) * k.blend};
}

u8 SampleM2U8(const wm2::AnimationTrack<u8>& track, const M2AnimTime& at, u8 def) {
    const M2KeySpan k = LocateM2Key(track, at);
    if (!k.valid)
        return def;
    return track.values[static_cast<usize>(k.sub)][static_cast<usize>(k.k0)];
}

u16 SampleM2U16(const wm2::AnimationTrack<u16>& track, const M2AnimTime& at, u16 def) {
    const M2KeySpan k = LocateM2Key(track, at);
    if (!k.valid)
        return def;
    return track.values[static_cast<usize>(k.sub)][static_cast<usize>(k.k0)];
}

std::string_view M2AnimationName(u16 animationId) {
    return ::whiteout::m2::animationName(animationId);
}

} // namespace whiteout::flakes::io
