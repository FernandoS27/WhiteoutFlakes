#pragma once

// ============================================================================
// The SC2 element vertex-shader math, on the CPU.
//
// Retail runs interpolation / twist / frame / spline math in the vertex shader
// (SC2_RIBBON_RE.md §4.3); this host renderer builds strips on the CPU, so the
// same math lives here. Element math, not ribbon math: `InterpolateValue`,
// `MakeRotation` and `CalculateDisplacementAndVelocity` are what a `PAR_`
// particle needs as much as a `RIB_` segment, which is why this lives in
// `sc2/` and both dialects include it (SC2_PARTICLE_DESIGN.md R6). It is a transcription of the shipped `.fx`
// source (`Ribbon.fx` + `RibbonParticleCommon.fx` + `VSElementUtils.fx` +
// `Common.fx` + `VSUtils.fx`) in the SOURCE's operation order — the O12 oracle
// gate (o12_vsmath.json, recorded from `refimpl_vs.py`, the same transcription
// in numpy.float32) replays these functions, so a re-ordered subexpression
// shows up as a red gate. sin/cos/exp go through the host libm, so trig/exp
// lanes carry a ≤2-ULP bound at the gate; everything else is bit-exact float32.
//
// The `M3_RIBBON` slang permutation implements the identical math on the GPU;
// keeping this in one header keeps the two in step.
// ============================================================================

#include "whiteout/flakes/types.h"

#include <array>
#include <cmath>

namespace whiteout::flakes::renderer::sc2::vs {

using whiteout::Vector3f;

// -- primitives (operation order matches the .fx source) ---------------------

inline f32 Saturate(f32 x) {
    return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x);
}

inline f32 Dot3(const Vector3f& a, const Vector3f& b) {
    return (a.x * b.x + a.y * b.y) + a.z * b.z; // left-to-right, per source
}

inline Vector3f Cross3(const Vector3f& a, const Vector3f& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

inline f32 Length3(const Vector3f& v) {
    return std::sqrt(Dot3(v, v));
}

inline Vector3f Scale(const Vector3f& v, f32 s) {
    return {v.x * s, v.y * s, v.z * s};
}

inline Vector3f Add(const Vector3f& a, const Vector3f& b) {
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}

inline Vector3f Sub(const Vector3f& a, const Vector3f& b) {
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}

inline Vector3f Normalize3(const Vector3f& v) {
    const f32 inv = 1.0f / Length3(v);
    return Scale(v, inv);
}

// RibbonParticleCommon.fx:34 — the 1e-5 length gate.
inline Vector3f SafeNormalize(const Vector3f& v, const Vector3f& d) {
    if (Length3(v) < 0.00001f)
        return d;
    return Normalize3(v);
}

inline f32 Lerp(f32 a, f32 b, f32 t) {
    return a + t * (b - a);
}
inline Vector3f Lerp(const Vector3f& a, const Vector3f& b, f32 t) {
    return Add(a, Scale(Sub(b, a), t));
}

// Common.fx:130
inline f32 SmoothStep(f32 x) {
    return x * x * (3.0f - 2.0f * x);
}

// Common.fx:136 / :147 — quadratic Bezier (scalar and float3 share the shape).
inline f32 Bezier(f32 v0, f32 v1, f32 v2, f32 t) {
    const f32 invT = 1.0f - t;
    f32 ret = (invT * invT) * v0;
    ret = ret + (2.0f * t * invT) * v1;
    ret = ret + (t * t) * v2;
    return ret;
}
inline Vector3f Bezier(const Vector3f& v0, const Vector3f& v1, const Vector3f& v2,
                       f32 t) {
    const f32 invT = 1.0f - t;
    Vector3f ret = Scale(v0, invT * invT);
    ret = Add(ret, Scale(v1, 2.0f * t * invT));
    ret = Add(ret, Scale(v2, t * t));
    return ret;
}

// -- RibbonParticleCommon.fx:42 (scalar) — sizes/alpha take mode 4 -----------
inline f32 InterpolateValue(f32 age, f32 v0, f32 v1, f32 v2, f32 mid, f32 invMid,
                            f32 hold, int mode) {
    f32 ret = v0;
    if (mode == 2) {
        ret = Bezier(v0, v1, v2, age);
    } else if (mode == 0) {
        if (age < mid)
            ret = Lerp(v0, v1, age * invMid);
        else
            ret = Lerp(v1, v2, (age - mid) / (1.0f - mid));
    } else if (mode == 1) {
        // NOT `Lerp(v0, v1, SmoothStep(s))`, and deliberately unlike the float3
        // overload below. The `.fx` source spells both `v0 + smoothstep(s)·(v1
        // − v0)`, and the shipped code does that for the vector evaluator
        // (`EvalAnimCurve2D` builds the factor first, then lerps four lanes)
        // while the SCALAR one groups it `((v1 − v0)·(s·s))·(3 − 2s) + v0`. In
        // float32 that is a different number on 17 of OP10's 530 mode-1
        // vectors — and identical in every other mode, which is why nothing
        // noticed. This renderer builds on the CPU, so the CPU function is the
        // authority; O12's own vectors do not separate the two groupings, so
        // the ribbon stays bit-exact through this change.
        if (age < mid) {
            const f32 s = age * invMid;
            ret = (((v1 - v0) * (s * s)) * ((s * -2.0f) + 3.0f)) + v0;
        } else {
            const f32 s = (age - mid) / (1.0f - mid);
            ret = (((v2 - v1) * (s * s)) * ((s * -2.0f) + 3.0f)) + v1;
        }
    } else if (mode == 3) {
        const f32 t0 = mid - hold;
        const f32 t1 = mid + hold;
        if (age < t0)
            ret = Lerp(v0, v1, age / t0);
        else if (age < t1)
            ret = v1;
        else
            ret = Lerp(v1, v2, (age - t1) / (1.0f - t1));
    } else if (mode == 4) {
        // Value substitution plus a re-normalised age. The clamp belongs to the
        // UPPER piece alone — the shipped code saturates there and hands the
        // lower piece its raw `age / (mid − hold)`. That only ever differs when
        // `hold > mid` makes the divisor negative, which is exactly what OP10's
        // (mid 0.1, hold 0.15) row is for.
        const f32 t0 = mid - hold;
        const f32 t1 = mid + hold;
        if (age < t0) {
            age = age / t0;
            v2 = v1;
        } else {
            age = Saturate((age - t1) / (1.0f - t1));
            v0 = v1;
        }
        ret = Bezier(v0, v1, v2, age);
    }
    return ret;
}

// -- RibbonParticleCommon.fx:118 (float3) — colours take mode 4 --------------
inline Vector3f InterpolateValue3(f32 age, const Vector3f& v0, const Vector3f& v1,
                                  const Vector3f& v2, f32 mid, f32 invMid,
                                  f32 hold, int mode) {
    Vector3f ret = v0;
    if (mode == 2) {
        ret = Bezier(v0, v1, v2, age);
    } else if (mode == 0) {
        if (age < mid)
            ret = Lerp(v0, v1, age * invMid);
        else
            ret = Lerp(v1, v2, (age - mid) / (1.0f - mid));
    } else if (mode == 1) {
        if (age < mid)
            ret = Lerp(v0, v1, SmoothStep(age * invMid));
        else
            ret = Lerp(v1, v2, SmoothStep((age - mid) / (1.0f - mid)));
    } else if (mode == 3) {
        const f32 t0 = mid - hold;
        const f32 t1 = mid + hold;
        if (age < t0)
            ret = Lerp(v0, v1, age / t0);
        else if (age < t1)
            ret = v1;
        else
            ret = Lerp(v1, v2, (age - t1) / (1.0f - t1));
    } else if (mode == 4) {
        // Quadratic with a lerped control point + a flat plateau (no saturate).
        const f32 t0 = mid - hold;
        const f32 t1 = mid + hold;
        if (age < t0)
            ret = Bezier(v0, Lerp(v0, v1, hold), v1, age / t0);
        else if (age < t1)
            ret = v1;
        else
            ret = Bezier(v1, Lerp(v1, v2, hold), v2, (age - t1) / (1.0f - t1));
    }
    return ret;
}

// -- Ribbon.fx:570 — twist: two-piece linear around rotationMidTime, radians -
inline f32 TwistAngle(f32 age, f32 rot0, f32 rot1, f32 rot2, f32 rotMid) {
    if (age < rotMid)
        return rot0 + (rot1 - rot0) * age / rotMid;
    return rot1 + (rot2 - rot1) * (age - rotMid) / (1.0f - rotMid);
}

// -- VSUtils.fx:25 — row-major half3x3 rotation about an axis -----------------
struct Mat3 {
    f32 m[3][3];
};

inline Mat3 MakeRotation(f32 angle, const Vector3f& axis) {
    const f32 s = std::sin(angle);
    const f32 c = std::cos(angle);
    const f32 oneC = 1.0f - c;
    Mat3 r{};
    r.m[0][0] = oneC * axis.x * axis.x + c;
    r.m[1][1] = oneC * axis.y * axis.y + c;
    r.m[2][2] = oneC * axis.z * axis.z + c;
    const f32 xy = axis.x * axis.y;
    const f32 zs = axis.z * s;
    r.m[1][0] = oneC * xy + zs;
    r.m[0][1] = oneC * xy - zs;
    const f32 zx = axis.z * axis.x;
    const f32 ys = axis.y * s;
    r.m[2][0] = oneC * zx - ys;
    r.m[0][2] = oneC * zx + ys;
    const f32 yz = axis.y * axis.z;
    const f32 xs = axis.x * s;
    r.m[2][1] = oneC * yz + xs;
    r.m[1][2] = oneC * yz - xs;
    return r;
}

// HLSL mul(rowVector, matrix): out[j] = sum_i v[i]*m[i][j], left to right.
inline Vector3f MulVecMat3(const Vector3f& v, const Mat3& m) {
    Vector3f out{};
    out.x = (v.x * m.m[0][0] + v.y * m.m[1][0]) + v.z * m.m[2][0];
    out.y = (v.x * m.m[0][1] + v.y * m.m[1][1]) + v.z * m.m[2][1];
    out.z = (v.x * m.m[0][2] + v.y * m.m[1][2]) + v.z * m.m[2][2];
    return out;
}

/// HLSL `mul(float4(p, 1), m).xyz` against a ROW-MAJOR float4x4, in the
/// shader's association: `((p.x*m0 + p.y*m1) + p.z*m2) + m3`.
///
/// A flat 16-float array rather than `Matrix44f` on purpose — the convention
/// is the whole content of this function, and a type that could be read either
/// way would hide it.
inline Vector3f MulPointMat4(const Vector3f& p, const std::array<f32, 16>& m) {
    Vector3f out{};
    out.x = ((p.x * m[0] + p.y * m[4]) + p.z * m[8]) + m[12];
    out.y = ((p.x * m[1] + p.y * m[5]) + p.z * m[9]) + m[13];
    out.z = ((p.x * m[2] + p.y * m[6]) + p.z * m[10]) + m[14];
    return out;
}

/// `mul(v, (float3x3)m)` — the upper-left 3×3, translation dropped.
inline Vector3f MulVecMat4As3(const Vector3f& v, const std::array<f32, 16>& m) {
    Mat3 r{};
    for (usize i = 0; i < 3; ++i)
        for (usize j = 0; j < 3; ++j)
            r.m[i][j] = m[4 * i + j];
    return MulVecMat3(v, r);
}

// -- VSElementUtils.fx:15 — the analytic exponential-drag closed form --------
// b_proceduralPosition (tech != 4) runs this in the VS, so it drives the
// rendered position of even the time-billboard technique: pos = birthPos +
// displacement(age). The CPU never integrates (Simulate_Type0 only appends and
// retires); this is where the motion lives.
struct DragResult {
    Vector3f displacement;
    Vector3f velocity;
};

inline DragResult CalculateDisplacementAndVelocity(f32 elapsed, const Vector3f& v0,
                                                   f32 mass, f32 invMass, f32 drag,
                                                   f32 invDrag, f32 gravity) {
    const Vector3f vGravity = {0.0f, 0.0f, gravity};
    const Vector3f vMg = Scale(vGravity, mass);
    const Vector3f vMgod = Scale(vMg, invDrag);
    const f32 expTerm = std::exp(-drag * invMass * elapsed);

    const Vector3f term0a = Scale(Add(v0, vMgod), -1.0f);
    const f32 term0b = mass * invDrag;
    const Vector3f term0 = Scale(Scale(term0a, term0b), expTerm);
    const Vector3f term1 = Scale(vMgod, elapsed);
    const Vector3f term2 = Scale(Add(v0, vMgod), term0b);

    DragResult r;
    r.displacement = Add(Sub(term0, term1), term2);
    const Vector3f velTerm0 =
        Scale(Add(Scale(v0, drag), vMg), invDrag * expTerm);
    r.velocity = Sub(velTerm0, vMgod);
    return r;
}

// -- Ribbon.fx:448/484 — fAge and the V coordinate ---------------------------
inline f32 FAge(f32 headU, f32 birthU, f32 deathU, f32 ageScalar) {
    const f32 a = (headU - birthU) / (deathU - birthU);
    return Saturate(a * ageScalar);
}

inline f32 VFromAge(f32 age, f32 y, f32 z) {
    // vUV.x = 1 − (y·fAge + z); the staged y = −1, z = 1 collapses to V = fAge.
    return 1.0f - (y * age + z);
}

// -- Ribbon.fx:535 — the frame construction ----------------------------------
struct Frame {
    Vector3f offset;   ///< per-vertex offset from the strip centreline.
    Vector3f normal;   ///< vVertexNormal.
    Vector3f tangent;  ///< vVertexTangent.
    Vector3f binormal; ///< vVertexBinormal.
};

/// `smoothPath` is Ribbon.fx:535's condition — true for
/// (!proceduralPosition || precomputedTangent || MIXED) on non-spline ribbons;
/// the else branch (:550) flattens billboards onto the camera plane. `cameraDir`
/// must already be in the ribbon's space.
inline Frame BuildFrame(int ribbonType, Vector3f tangent, const Vector3f& up,
                        const Vector3f& cameraDir, f32 offsetX, f32 offsetY,
                        f32 angle, bool smoothPath) {
    tangent = SafeNormalize(tangent, Vector3f{1, 0, 0});

    Vector3f normal;
    Vector3f binormal;
    if (smoothPath) {
        if (ribbonType == 0) { // RIBBON_BILLBOARD
            normal = Scale(cameraDir, -1.0f);
            binormal = SafeNormalize(Cross3(normal, tangent), Vector3f{0, 1, 0});
        } else {
            binormal = up;
            normal = SafeNormalize(Cross3(tangent, binormal), Vector3f{0, 0, 1});
        }
    } else {
        if (ribbonType == 0) {
            const Vector3f seg = Normalize3(
                Sub(tangent, Scale(cameraDir, Dot3(tangent, cameraDir))));
            binormal = Cross3(seg, cameraDir);
        } else {
            binormal = SafeNormalize(Cross3(tangent, up), Vector3f{0, 1, 0});
        }
        normal = SafeNormalize(Cross3(tangent, binormal), Vector3f{0, 0, 1});
    }

    const Mat3 rot = MakeRotation(angle, tangent);
    binormal = MulVecMat3(binormal, rot);
    normal = MulVecMat3(normal, rot);

    Frame f;
    if (ribbonType == 0 || ribbonType == 1) { // billboard / planar
        f.offset = Scale(binormal, offsetY);
        f.normal = normal;
        f.tangent = Scale(binormal, -1.0f);
        f.binormal = Scale(tangent, -1.0f);
    } else { // cylinder / star
        // basis rows {normal, tangent, binormal}; offset = (offsetX,0,offsetY)·basis.
        Mat3 basis{};
        basis.m[0][0] = normal.x; basis.m[0][1] = normal.y; basis.m[0][2] = normal.z;
        basis.m[1][0] = tangent.x; basis.m[1][1] = tangent.y; basis.m[1][2] = tangent.z;
        basis.m[2][0] = binormal.x; basis.m[2][1] = binormal.y; basis.m[2][2] = binormal.z;
        f.offset = MulVecMat3(Vector3f{offsetX, 0.0f, offsetY}, basis);
        if (ribbonType == 2)
            f.offset = SafeNormalize(f.offset, Vector3f{0, 0, 1});
        f.normal = f.offset;
        f.tangent = tangent;
        f.binormal = Cross3(f.tangent, f.normal);
    }
    return f;
}

} // namespace whiteout::flakes::renderer::sc2::vs
