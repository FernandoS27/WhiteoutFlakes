#pragma once

// ============================================================================
// The orientation frames `Particle_BuildOrientationBasis` @0x71000BAB30 builds.
//
// All fourteen render modes pick an axis, cross it with world up (Z-up) and back,
// and read the triple out as matrix COLUMNS; only the column order differs, and
// getting it wrong is a 120-degree relabel of the quad. Recovered by execution
// (G-D3P-11). See D3_PARTICLE_DESIGN.md §30.1.
// ============================================================================

#include "renderer/particle/d3/d3_channels.h"  // PrtRenderMode
#include "renderer/particle/d3/d3_particle.h" // OrientationFromAxes, the arc modes 9 and 10 compose
#include "types.h"
#include "whiteout/flakes/types.h"

#include <cmath>

namespace whiteout::flakes::renderer::particle::d3 {

/// The engine's degenerate-frame epsilon. Pinned by execution: at 1e-7 sixty of
/// G-D3P-11's cases go red.
inline constexpr f32 kFrameEpsilon = kEpsilon;

inline constexpr Vector3f kWorldUp{0.0f, 0.0f, 1.0f};
inline constexpr Vector3f kWorldX{1.0f, 0.0f, 0.0f};
inline constexpr Vector3f kWorldY{0.0f, 1.0f, 0.0f};

inline Vector3f FrameCross(const Vector3f& a, const Vector3f& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

/// z^2 + (x^2 + y^2) — the engine's summation order for a frame vector. The
/// candidate-length test below uses the other one, and they are not the same float.
inline f32 FrameLength(const Vector3f& v) {
    return std::sqrt(v.z * v.z + (v.x * v.x + v.y * v.y));
}

inline f32 CandidateLengthSq(const Vector3f& v) {
    return (v.x * v.x + v.y * v.y) + v.z * v.z;
}

inline Vector3f FrameNormalise(const Vector3f& v, f32 length) {
    const f32 inv = 1.0f / length;
    return {v.x * inv, v.y * inv, v.z * inv};
}

/// What every arm that builds its own basis computes, step 2 above.
struct TwoCrossFrame {
    Vector3f c; ///< the first cross, normalised
    Vector3f d; ///< the cross back, normalised only when it has length
};

/// `c = normalise(a × b)`, false when that collapses; then `d = s × c`, or
/// `c × s` under @p cFirst, normalised only when it has length.
///
/// The operand order is each arm's own and is passed, not normalised away:
/// `x × y` and `-(y × x)` are the same vector except in the sign of a zero,
/// and a zero's sign survives into the quaternion.
inline bool TwoCross(const Vector3f& a, const Vector3f& b, const Vector3f& s, bool cFirst,
                     TwoCrossFrame& out) {
    const Vector3f c0 = FrameCross(a, b);
    const f32 cl = FrameLength(c0);
    if (cl <= kFrameEpsilon)
        return false;
    out.c = FrameNormalise(c0, cl);
    out.d = cFirst ? FrameCross(out.c, s) : FrameCross(s, out.c);
    const f32 dl = FrameLength(out.d);
    if (dl > kFrameEpsilon)
        out.d = FrameNormalise(out.d, dl);
    return true;
}

/// Branch-on-largest-diagonal, from a matrix given by its COLUMNS.
///
/// Inlined at five sites in the original and identical at every one. The trace
/// test is `sum + 1.0f >= 1.0f` rather than `sum >= 0.0f`; near zero those are
/// different questions and the engine asks the first.
inline Quaternion QuatFromBasis(const Vector3f& c0, const Vector3f& c1, const Vector3f& c2) {
    const f32 m00 = c0.x, m01 = c1.x, m02 = c2.x;
    const f32 m10 = c0.y, m11 = c1.y, m12 = c2.y;
    const f32 m20 = c0.z, m21 = c1.z, m22 = c2.z;

    f32 x, y, z, w;
    const f32 tr = (m11 + m00 + m22) + 1.0f;
    if (tr >= 1.0f) {
        x = m21 - m12;
        y = m02 - m20;
        z = m10 - m01;
        w = tr;
    } else if (m11 > m00 || m22 > m00) {
        if (m11 < m00 || m11 < m22) {
            x = m02 + m20;
            y = m21 + m12;
            z = ((-m00 + m22) - m11) + 1.0f;
            w = m10 - m01;
        } else {
            x = m01 + m10;
            y = (-m00 + (m11 - m22)) + 1.0f;
            z = m21 + m12;
            w = m02 - m20;
        }
    } else {
        x = ((m00 - m11) - m22) + 1.0f;
        y = m01 + m10;
        z = m02 + m20;
        w = m21 - m12;
    }
    const f32 n = 1.0f / std::sqrt(w * w + (z * z + (y * y + x * x)));
    return {x * n, y * n, z * n, w * n};
}

/// Render mode 0, and only when the system's flag bit 13 is set.
///
/// The axis is used AS GIVEN — it is not normalised first, so its length reaches
/// the frame. Columns are (-axis, axis x up, (-axis) x that). False means the
/// engine wrote nothing and the caller's quaternion stands.
inline bool OrientBillboard(const Vector3f& axis, Quaternion& out) {
    const Vector3f u{-axis.x, -axis.y, -axis.z};
    TwoCrossFrame f;
    if (!TwoCross(axis, kWorldUp, u, false, f))
        return false;
    out = QuatFromBasis(u, f.c, f.d);
    return true;
}

/// The three-way pick at the head of `Particle_SelectOrientationAxis`
/// @0x71000BA5A0 (render modes 3, 4, 5 and 6) and of the mode-2 helper: the raw
/// candidate, else the stored unit vector, else world X — which keeps a
/// never-moved particle off a NaN frame (§30.1).
inline Vector3f SelectFrameAxis(const Vector3f& candidate, const Vector3f& fallback) {
    Vector3f n = candidate;
    if (CandidateLengthSq(n) >= kFrameEpsilon) {
        const f32 length = std::sqrt(CandidateLengthSq(n));
        if (length > kFrameEpsilon)
            n = FrameNormalise(n, length);
        return n;
    }
    n = fallback;
    if (CandidateLengthSq(n) < kFrameEpsilon)
        n = kWorldX;
    return n;
}

/// The frame the ungated modes leave, as the right/up pair a CPU-built quad
/// needs. Column order is (right, up, normal), which modes 6 and 13 prove. Only
/// the ungated arms build one: the gate (`sys+8` bit 13) is set only for the
/// child-actor types. Modes 0, 1 and 8 return false. See §30.1.
struct QuadFrame {
    Vector3f right{1.0f, 0.0f, 0.0f};
    Vector3f up{0.0f, 0.0f, 1.0f};
};

/// Columns `(-c, b, n)`: the axis is the quad's NORMAL, so the quad stands
/// ACROSS its own direction — `Particle_SelectOrientationAxis`'s column order,
/// with the selected axis in column 2 rather than column 0. Modes 3, 4, 5 and
/// 6; G-D3P-11 replays it through `BuildQuadFrame` to the bit.
inline bool FrameAcrossAxis(const Vector3f& n, QuadFrame& out) {
    TwoCrossFrame f;
    if (!TwoCross(n, kWorldUp, n, true, f))
        return false;
    out.right = {-f.c.x, -f.c.y, -f.c.z};
    out.up = f.d;
    return true;
}

/// A spinning particle's birth orientation: `ParticleSystem_EmitParticle`
/// @0x71000B23B0 calls `Particle_SelectOrientationAxis` ungated with the negated
/// camera direction as the candidate and the birth unit axis as the fallback,
/// into particle+216. The same `(-c, b, n)` frame as the across-axis modes, as a
/// quaternion; false leaves the seat standing, as the engine does.
inline bool SpinBirthSeat(const Vector3f& camForward, const Vector3f& axisUnit, Quaternion& out) {
    const Vector3f n = SelectFrameAxis({-camForward.x, -camForward.y, -camForward.z}, axisUnit);
    TwoCrossFrame f;
    if (!TwoCross(n, kWorldUp, n, true, f))
        return false;
    out = QuatFromBasis({-f.c.x, -f.c.y, -f.c.z}, f.d, n);
    return true;
}

/// Columns `(±c, n, b)`: the axis is the quad's UP, so the quad stretches ALONG
/// it and @p about resolves the one remaining degree of freedom — the camera for
/// modes 2 and 11, world up for mode 12. Returning false when `n × about`
/// collapses is a deliberate deviation from the engine (§30.1).
inline bool FrameAlongAxis(const Vector3f& n, const Vector3f& about, bool negateRight,
                           QuadFrame& out) {
    const Vector3f c0 = FrameCross(n, about);
    const f32 cl = FrameLength(c0);
    if (cl <= kFrameEpsilon)
        return false;
    const Vector3f c = FrameNormalise(c0, cl);
    out.right = negateRight ? Vector3f{-c.x, -c.y, -c.z} : c;
    out.up = n;
    return true;
}

/// Mode 13 read out: up is world Z and the quad yaws to face the camera. The
/// engine's ungated `(c, b, u)` frame without its second normalise, so G-D3P-11
/// holds it to 1e-5 rather than to the bit (§30.1).
inline bool FrameVertical(const Vector3f& camForward, QuadFrame& out) {
    const f32 length = std::sqrt((camForward.x * camForward.x + camForward.y * camForward.y) + 0.0f);
    if (length <= kFrameEpsilon)
        return false;
    const f32 inv = 1.0f / length;
    out.right = {camForward.y * inv, -camForward.x * inv, 0.0f};
    out.up = kWorldUp;
    return true;
}

/// Mode 7 — the EMITTER's own frame, columns `(q*Y, q*Z, q*X)`: an unrotated
/// emitter gives a quad in the world YZ plane facing +X. Rotating only the two
/// axes needed is the engine's answer without its round trip (§30.1).
inline bool FrameEmitter(const Quaternion& q, QuadFrame& out) {
    out.right = q.rotate_vector(kWorldY);
    out.up = q.rotate_vector(kWorldUp);
    return true;
}

/// Modes 9 and 10 — ground-conforming. The emitter frame turned so that its own
/// up lands on the terrain normal, then read out as `(R*X, R*Y, R*Z)`: an
/// identity `R` lies FLAT (§30.1).
inline bool FrameGroundConformed(const Quaternion& q, const Vector3f& groundNormal,
                                 QuadFrame& out) {
    const Quaternion arc = OrientationFromAxes(q.rotate_vector(kWorldUp), groundNormal);
    const Quaternion r = arc * q;
    out.right = r.rotate_vector(kWorldX);
    out.up = r.rotate_vector(kWorldY);
    return true;
}

/// Everything `Particle_BuildOrientationBasis` reads, in the caller's terms.
struct FrameInput {
    /// The camera's view direction. `Particle_PrepareDrawFrame` passes
    /// `view+0x28C` — the same vector the particle draw list sorts on, which is
    /// what identifies it.
    Vector3f camForward{0.0f, 1.0f, 0.0f};
    Vector3f axis{0.0f, 0.0f, 0.0f};     ///< pool+444, the frame displacement
    Vector3f axisUnit{0.0f, 0.0f, 0.0f}; ///< pool+456, its last unit direction
    /// Particle position minus the system's reference point (sys+64).
    Vector3f fromSystem{0.0f, 0.0f, 0.0f};
    /// pool+560, the terrain normal cached under this particle. The engine
    /// raycasts for it and defaults to world up on a miss.
    Vector3f groundNormal{0.0f, 0.0f, 1.0f};
    /// pool+488, the emitter's orientation frozen onto the particle at birth.
    Quaternion emitterQuat = Quaternion::identity();
};

/// All fourteen modes. False means the mode writes nothing — 0 (ungated), 1 and
/// 8 — or that the frame came out degenerate; either way the caller's own basis
/// stands.
inline bool BuildQuadFrame(PrtRenderMode renderMode, const FrameInput& in, QuadFrame& out) {
    using enum PrtRenderMode;
    switch (renderMode) {
    case AxisStreak:
        return FrameAlongAxis(SelectFrameAxis(in.axis, in.axisUnit), in.camForward, true, out);
    case AcrossAxis:
        return FrameAcrossAxis(SelectFrameAxis(in.axis, in.axisUnit), out);
    case AcrossSystemXY:
        return FrameAcrossAxis(
            SelectFrameAxis({in.fromSystem.x, in.fromSystem.y, 0.0f}, in.axisUnit), out);
    case AcrossSystem:
        return FrameAcrossAxis(SelectFrameAxis(in.fromSystem, in.axisUnit), out);
    case AcrossCameraXY:
        return FrameAcrossAxis(
            SelectFrameAxis({-in.camForward.x, -in.camForward.y, 0.0f}, in.axisUnit), out);
    case EmitterFrame:
        return FrameEmitter(in.emitterQuat, out);
    case Ground:
    case GroundAlt:
        return FrameGroundConformed(in.emitterQuat, in.groundNormal, out);
    case SystemStreak:
        return FrameAlongAxis(SelectFrameAxis(in.fromSystem, in.axisUnit), in.camForward, true,
                              out);
    case AxisUpright:
        return FrameAlongAxis(SelectFrameAxis(in.axis, in.axisUnit), kWorldUp, false, out);
    case Vertical:
        return FrameVertical(in.camForward, out);
    default:
        return false;
    }
}

// ---------------------------------------------------------------------------
// The GATED arm — what a spawned child actor gets
// ---------------------------------------------------------------------------

/// The gated column order `(n, up x n, n x (up x n))` as a quaternion — the
/// cyclic shift of the ungated `(-c, b, n)`, so a Z-up +X-forward model turns its
/// +X onto @p n. `Particle_QuaternionFromAxes` @0x71000BD2B0 reduces to
/// `QuatFromBasis(n, c, b)`. @p n is used as given. See §30.1.
inline bool GatedFrame(const Vector3f& n, Quaternion& out) {
    TwoCrossFrame f;
    if (!TwoCross(kWorldUp, n, n, false, f))
        return false;
    out = QuatFromBasis(n, f.c, f.d);
    return true;
}

/// The gated arm of the across-axis modes 3, 4, 5 and 6, which is NOT the
/// cyclic shift: columns `((up x n) x n, up x n, n)`, a quarter turn about @p n
/// against the ungated frame. Pinned to the bit by G-D3P-11 (F17, §30.1).
inline bool GatedFrameAcross(const Vector3f& n, Quaternion& out) {
    TwoCrossFrame f;
    if (!TwoCross(kWorldUp, n, n, true, f))
        return false;
    out = QuatFromBasis(f.d, f.c, n);
    return true;
}

/// Mode 13's axis, in the engine's own arithmetic: the camera direction
/// negated and flattened onto XY. The `+ 0.0f` in the length and the `inv *
/// 0.0f` z are the original's, kept so the two arms of mode 13 agree.
inline bool FlattenedCameraAxis(const Vector3f& camForward, Vector3f& out) {
    const f32 length =
        std::sqrt((camForward.x * camForward.x + camForward.y * camForward.y) + 0.0f);
    if (length <= kFrameEpsilon)
        return false;
    const f32 inv = 1.0f / length;
    out = {-(camForward.x * inv), -(camForward.y * inv), inv * 0.0f};
    return true;
}

/// The quaternion `Actor_SpawnFromSno` receives, per render mode: the same
/// switch read through its gated arm, which `ParticleSystem_EmitParticle`
/// @0x71000B1A00 runs AT EMIT. False leaves @p out (the system's own quaternion)
/// untouched. See §30.1.
inline bool BuildChildOrientation(PrtRenderMode renderMode, const FrameInput& in,
                                  Quaternion& out) {
    using enum PrtRenderMode;
    switch (renderMode) {
    case CameraGated:
        // The one mode with no ungated arm at all. `OrientBillboard` is the
        // gated construction with the negation folded in, and the axis crossed
        // on the other side.
        return OrientBillboard(in.camForward, out);
    case AxisStreak:
    case AxisUpright:
        return GatedFrame(SelectFrameAxis(in.axis, in.axisUnit), out);
    case SystemStreak:
        // Modes 2 and 11 lose the camera: `Particle_OrientationBasisHelper`
        // @0x71000BBF10 returns through the gated quaternion first (§30.1).
        return GatedFrame(SelectFrameAxis(in.fromSystem, in.axisUnit), out);
    case AcrossAxis:
        return GatedFrameAcross(SelectFrameAxis(in.axis, in.axisUnit), out);
    case AcrossSystemXY:
        return GatedFrameAcross(
            SelectFrameAxis({in.fromSystem.x, in.fromSystem.y, 0.0f}, in.axisUnit), out);
    case AcrossSystem:
        return GatedFrameAcross(SelectFrameAxis(in.fromSystem, in.axisUnit), out);
    case AcrossCameraXY:
        return GatedFrameAcross(
            SelectFrameAxis({-in.camForward.x, -in.camForward.y, 0.0f}, in.axisUnit), out);
    case EmitterFrame:
        out = in.emitterQuat;
        return true;
    case Ground:
    case GroundAlt:
        // The arm the ungated path reaches the long way round: it rotates the
        // three world axes by this same product and reads them back as the
        // columns, which is the product again.
        out = OrientationFromAxes(in.emitterQuat.rotate_vector(kWorldUp), in.groundNormal) *
              in.emitterQuat;
        return true;
    case Vertical: {
        Vector3f n;
        if (!FlattenedCameraAxis(in.camForward, n))
            return false;
        return GatedFrame(n, out);
    }
    default:
        return false;
    }
}

} // namespace whiteout::flakes::renderer::particle::d3
