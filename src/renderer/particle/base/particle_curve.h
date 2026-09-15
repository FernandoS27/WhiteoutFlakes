#pragma once

// ============================================================================
// Lifetime curves — colour, alpha, size and rotation from birth (t = 0) to death
// (t = 1): any number of keys in normalised time, each curve its own interp.
// Two WC3 quirks are data: `bias` (its `t = raw*0.99 + 0.005` in-segment skew;
// zero elsewhere) and cell animation as a wrap-and-repeat track type.
// ============================================================================

#include "types.h"
#include "whiteout/flakes/types.h"

#include <vector>

namespace whiteout::flakes::renderer::particle {

enum class Interp : u8 {
    Step,       // hold keys[i] until keys[i+1]
    Linear,     // MDX "linear", M2's usual mode
    Hermite,    // MDX "hermite" — uses in/out tangents
    Bezier,     // MDX "bezier"  — tangents are control points
    CatmullRom, // tangents derived from neighbours; M3 splines
};

// Component-wise helpers so a curve can carry f32, Vector2f or Vector3f without
// depending on which operators the vector library happens to expose.
inline f32 CurveMul(f32 v, f32 s) {
    return v * s;
}
inline f32 CurveAdd(f32 a, f32 b) {
    return a + b;
}
inline Vector2f CurveMul(const Vector2f& v, f32 s) {
    return {v.x * s, v.y * s};
}
inline Vector2f CurveAdd(const Vector2f& a, const Vector2f& b) {
    return {a.x + b.x, a.y + b.y};
}
inline Vector3f CurveMul(const Vector3f& v, f32 s) {
    return {v.x * s, v.y * s, v.z * s};
}
inline Vector3f CurveAdd(const Vector3f& a, const Vector3f& b) {
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}

template <class T>
struct CurveKey {
    f32 t = 0.0f;
    T value{};
    T inTangent{};  // Hermite / Bezier only
    T outTangent{}; // Hermite / Bezier only
};

// Locate the segment containing `t`, using `hint` as a starting guess. The hint
// is an optimisation only: the result is identical for any hint value, which is
// what tests/particle_curve_test.cpp asserts.
template <class KeyVec>
usize FindSegment(const KeyVec& keys, f32 t, u32 hint) {
    const usize last = keys.size() - 2; // index of the final segment
    usize i = (hint <= last) ? static_cast<usize>(hint) : 0;
    // Forward `>` and backward `<=` agree: a sample exactly on a key time resolves
    // to the earlier segment whatever the hint. Mismatched, the result would be
    // hint-dependent — and select different sprite cells at a boundary.
    while (i < last && t > keys[i + 1].t)
        ++i;
    while (i > 0 && t <= keys[i].t)
        --i;
    return i;
}

// Map a raw in-segment position through the sampling bias. WC3 never samples
// the exact endpoints of a segment; `bias` reproduces that inset.
inline f32 ApplyBias(f32 s, f32 bias) {
    if (s < 0.0f)
        s = 0.0f;
    if (s > 1.0f)
        s = 1.0f;
    return (bias > 0.0f) ? (s * (1.0f - 2.0f * bias) + bias) : s;
}

template <class T>
class ParticleCurve {
public:
    void SetInterp(Interp i) {
        interp_ = i;
    }
    void SetBias(f32 b) {
        bias_ = b;
    }
    void AddKey(f32 t, const T& v) {
        keys_.push_back(CurveKey<T>{t, v, T{}, T{}});
    }
    void AddKey(const CurveKey<T>& k) {
        keys_.push_back(k);
    }

    bool Empty() const {
        return keys_.empty();
    }
    usize KeyCount() const {
        return keys_.size();
    }
    usize SegmentCount() const {
        return keys_.size() > 1 ? keys_.size() - 1 : 0;
    }
    f32 KeyTime(usize i) const {
        return keys_[i].t;
    }

    T Evaluate(f32 t, u32 hint = 0) const {
        if (keys_.empty())
            return T{};
        if (keys_.size() == 1)
            return keys_[0].value;
        if (t <= keys_.front().t && bias_ <= 0.0f)
            return keys_.front().value;
        if (t >= keys_.back().t && bias_ <= 0.0f)
            return keys_.back().value;

        const usize i = FindSegment(keys_, t, hint);
        const CurveKey<T>& a = keys_[i];
        const CurveKey<T>& b = keys_[i + 1];

        const f32 span = b.t - a.t;
        const f32 raw = (span > 0.0f) ? (t - a.t) / span : 0.0f;
        const f32 s = ApplyBias(raw, bias_);

        switch (interp_) {
        case Interp::Step:
            return a.value;
        case Interp::Hermite:
            return Hermite(a.value, a.outTangent, b.value, b.inTangent, s);
        case Interp::Bezier:
            return Bezier(a.value, a.outTangent, b.inTangent, b.value, s);
        case Interp::CatmullRom:
            return CatmullRom(i, s);
        case Interp::Linear:
        default:
            return Lerp(a.value, b.value, s);
        }
    }

private:
    static T Lerp(const T& a, const T& b, f32 s) {
        return CurveAdd(a, CurveMul(CurveAdd(b, CurveMul(a, -1.0f)), s));
    }

    static T Hermite(const T& p0, const T& m0, const T& p1, const T& m1, f32 s) {
        const f32 s2 = s * s, s3 = s2 * s;
        const f32 h00 = 2 * s3 - 3 * s2 + 1;
        const f32 h10 = s3 - 2 * s2 + s;
        const f32 h01 = -2 * s3 + 3 * s2;
        const f32 h11 = s3 - s2;
        return CurveAdd(CurveAdd(CurveMul(p0, h00), CurveMul(m0, h10)),
                        CurveAdd(CurveMul(p1, h01), CurveMul(m1, h11)));
    }

    static T Bezier(const T& p0, const T& c0, const T& c1, const T& p1, f32 s) {
        const f32 u = 1.0f - s;
        const f32 b0 = u * u * u;
        const f32 b1 = 3 * u * u * s;
        const f32 b2 = 3 * u * s * s;
        const f32 b3 = s * s * s;
        return CurveAdd(CurveAdd(CurveMul(p0, b0), CurveMul(c0, b1)),
                        CurveAdd(CurveMul(c1, b2), CurveMul(p1, b3)));
    }

    // Tangents from the neighbouring keys; endpoints fall back to the adjacent
    // segment so the curve stays C1 inside and merely linear at the ends.
    T CatmullRom(usize i, f32 s) const {
        const T& p1 = keys_[i].value;
        const T& p2 = keys_[i + 1].value;
        const T& p0 = (i > 0) ? keys_[i - 1].value : p1;
        const T& p3 = (i + 2 < keys_.size()) ? keys_[i + 2].value : p2;
        const T m0 = CurveMul(CurveAdd(p2, CurveMul(p0, -1.0f)), 0.5f);
        const T m1 = CurveMul(CurveAdd(p3, CurveMul(p1, -1.0f)), 0.5f);
        return Hermite(p1, m0, p2, m1, s);
    }

    Interp interp_ = Interp::Linear;
    f32 bias_ = 0.0f;
    std::vector<CurveKey<T>> keys_;
};

// Sprite-sheet cell animation. Each segment sweeps an integer cell range,
// optionally repeating within the segment — wrap-and-repeat semantics, not
// value interpolation, which is why this is not a ParticleCurve.
struct CellSegment {
    f32 endT = 1.0f;
    i32 start = 0;
    i32 end = 0;
    i32 repeat = 1;
};

class CellAnimTrack {
public:
    void SetBias(f32 b) {
        bias_ = b;
    }
    void AddSegment(f32 endT, i32 start, i32 end, i32 repeat) {
        segments_.push_back({endT, start, end, repeat});
    }
    bool Empty() const {
        return segments_.empty();
    }

    i32 Evaluate(f32 t, u32 hint = 0) const;

private:
    f32 bias_ = 0.0f;
    std::vector<CellSegment> segments_;
};

struct LifetimeCurves {
    ParticleCurve<Vector3f> color; // linear RGB, 0..1
    ParticleCurve<f32> alpha;      // 0..1
    ParticleCurve<Vector2f> size;  // non-uniform; WC3 sets x == y

    CellAnimTrack headCells;
    CellAnimTrack tailCells;
};

} // namespace whiteout::flakes::renderer::particle
