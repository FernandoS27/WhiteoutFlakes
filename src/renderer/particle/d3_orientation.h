#pragma once

// ============================================================================
// The orientation frames `Particle_BuildOrientationBasis` @0x71000BAB30 builds.
//
// Recovered by executing the original — see G-D3P-11 in
// `tools/d3_particle_oracle/gate_a7b.py`, 3360 cases. Fourteen render modes, and
// all fourteen are the same three steps:
//
//   1. pick an axis (a caller vector, a direction to a reference point, or a
//      candidate with a two-step fallback),
//   2. cross it with world up and cross back to get an orthonormal triple,
//   3. read the triple out as the COLUMNS of a rotation matrix and convert.
//
// The only per-mode difference is which vector lands in which column, so the
// conversion below is written once and every mode passes its own column order.
// Getting that permutation wrong is not a small error: it is a 120-degree relabel
// of the quad's own axes.
//
// Two facts worth keeping in view:
//
// * The engine's world basis is Z-up — the three globals it reads are (0,0,1),
//   (1,0,0) and (0,1,0), bound by the loader's relocations, not by a view. There
//   is no camera anywhere in this function.
// * Modes 1 and 8 write nothing at all, and mode 0 writes nothing unless the
//   system's runtime flag bit 13 is set — which happens only for the child-actor
//   types (see `BuildQuadFrame`). Those three account for 13,117 of the corpus's
//   render-mode appearances, so for most content this whole family is a no-op and
//   the particle keeps the orientation it already had.
//
// What is NOT recovered here is how the shader turns the quaternion into corner
// positions: `Particle_WriteQuadVertices` emits a local 2-D corner pair and the
// expansion happens on the GPU, which this audit cannot reach. So these build the
// frame the engine builds, and choosing which column is "right" and which is the
// normal is still a rendering decision.
// ============================================================================

#include "d3_channels.h"  // PrtRenderMode
#include "d3_particle.h" // OrientationFromAxes, the arc modes 9 and 10 compose
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
    const Vector3f c0 = FrameCross(axis, kWorldUp);
    const f32 cl = FrameLength(c0);
    if (cl <= kFrameEpsilon)
        return false;
    const Vector3f c = FrameNormalise(c0, cl);
    const Vector3f u{-axis.x, -axis.y, -axis.z};
    Vector3f b = FrameCross(u, c);
    const f32 bl = FrameLength(b);
    if (bl > kFrameEpsilon)
        b = FrameNormalise(b, bl);
    out = QuatFromBasis(u, c, b);
    return true;
}

/// Render mode 13 — the axis flattened onto XY and normalised, which is what makes
/// this the world-vertical-locked quad. The gate bit permutes the columns.
inline bool OrientFlattened(const Vector3f& axis, bool gated, Quaternion& out) {
    const f32 length = std::sqrt((axis.x * axis.x + axis.y * axis.y) + 0.0f);
    if (length <= kFrameEpsilon)
        return false;
    const f32 inv = 1.0f / length;
    const Vector3f u{-(axis.x * inv), -(axis.y * inv), inv * 0.0f};
    const Vector3f c0 = FrameCross(kWorldUp, u);
    const f32 cl = FrameLength(c0);
    if (cl <= kFrameEpsilon)
        return false;
    const Vector3f c = FrameNormalise(c0, cl);
    Vector3f b = FrameCross(u, c);
    const f32 bl = FrameLength(b);
    if (bl > kFrameEpsilon)
        b = FrameNormalise(b, bl);
    out = gated ? QuatFromBasis(u, c, b) : QuatFromBasis(c, b, u);
    return true;
}

/// The three-way pick at the head of `Particle_SelectOrientationAxis` and of the
/// mode-2 helper: the raw candidate, else the stored unit vector, else world X.
///
/// The two candidates are a particle's frame displacement and its last non-zero
/// direction, so a particle that has stopped keeps pointing where it was going,
/// and only one that has never moved falls through to world X.
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

/// `Particle_SelectOrientationAxis` @0x71000BA5A0 — render modes 3, 4, 5 and 6.
///
/// The three-way fallback (candidate, else fallback, else world X) is what keeps a
/// zero-velocity particle from getting a NaN frame. Columns are (-c, b, axis), so
/// here the selected axis is column 2 rather than column 0.
inline bool OrientFromAxis(const Vector3f& candidate, const Vector3f& fallback,
                           Quaternion& out) {
    const Vector3f n = SelectFrameAxis(candidate, fallback);
    const Vector3f c0 = FrameCross(n, kWorldUp);
    const f32 cl = FrameLength(c0);
    if (cl <= kFrameEpsilon)
        return false;
    const Vector3f c = FrameNormalise(c0, cl);
    Vector3f b = FrameCross(c, n);
    const f32 bl = FrameLength(b);
    if (bl > kFrameEpsilon)
        b = FrameNormalise(b, bl);
    out = QuatFromBasis({-c.x, -c.y, -c.z}, b, n);
    return true;
}

/// The frame the ungated modes leave, as the right/up pair a CPU-built quad
/// needs.
///
/// The header above says the column-to-axis mapping is "still a rendering
/// decision". It is not: **column order is (right, up, normal)**, and the corpus
/// proves it. Modes 6 and 13 are two different constructions — mode 6 takes the
/// negated camera direction through `Particle_SelectOrientationAxis`'s column
/// order `(-c, b, n)`, mode 13 takes the same vector flattened through its own
/// `(c, b, u)` — and they agree vector for vector, right for right and up for up,
/// only under this reading. The gate bit's second column order is the cyclic
/// shift that keeps the same quad, which is why it can differ per mode at all.
///
/// Only the ungated arms are here, and that is exact rather than approximate.
/// The gate is bit 13 of the system's RUNTIME word at `sys+8`, not of the SNO's
/// `dwFlags`, and `ParticleSystem_Spawn` @0x71000AC504 sets it for
/// `eSystemType` 1, 3 and 4 — `CMP W8, #4` then `(1 << type) & 0x1A` — which are
/// the three types that spawn CHILD ACTORS instead of particles. So the gated
/// column order orients a spawned model and the ungated one orients a quad;
/// nothing that reaches a billboard ever takes it.
///
/// Modes 0, 1 and 8 write nothing (60.7% of the corpus) and return false, which
/// leaves the caller's basis standing. The other eleven are all here.
struct QuadFrame {
    Vector3f right{1.0f, 0.0f, 0.0f};
    Vector3f up{0.0f, 0.0f, 1.0f};
};

/// Columns `(-c, b, n)`: the axis is the quad's NORMAL, so the quad stands
/// ACROSS its own direction. Modes 3, 4, 5 and 6.
inline bool FrameAcrossAxis(const Vector3f& n, QuadFrame& out) {
    const Vector3f c0 = FrameCross(n, kWorldUp);
    const f32 cl = FrameLength(c0);
    if (cl <= kFrameEpsilon)
        return false;
    const Vector3f c = FrameNormalise(c0, cl);
    Vector3f b = FrameCross(c, n);
    const f32 bl = FrameLength(b);
    if (bl > kFrameEpsilon)
        b = FrameNormalise(b, bl);
    out.right = {-c.x, -c.y, -c.z};
    out.up = b;
    return true;
}

/// Columns `(±c, n, b)`: the axis is the quad's UP, so the quad stretches ALONG
/// it and @p about resolves the one remaining degree of freedom — the camera for
/// modes 2 and 11, world up for mode 12.
///
/// The engine does not bail when `n × about` collapses; it carries the near-zero
/// vector into a degenerate matrix and normalises whatever quaternion falls out.
/// Returning false instead is the one deliberate deviation here: the case is a
/// bolt travelling exactly at the camera (2) or exactly upward (12), and a
/// camera-facing quad beats a random one.
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

/// Mode 13, `OrientFlattened` read out: up is world Z and the quad yaws to face
/// the camera. The vertical billboard a bolt, a beam or a column of flame wants —
/// and the mode this build used to draw flat on the ground.
inline bool FrameVertical(const Vector3f& camForward, QuadFrame& out) {
    const f32 length = std::sqrt((camForward.x * camForward.x + camForward.y * camForward.y) + 0.0f);
    if (length <= kFrameEpsilon)
        return false;
    const f32 inv = 1.0f / length;
    out.right = {camForward.y * inv, -camForward.x * inv, 0.0f};
    out.up = kWorldUp;
    return true;
}

/// Mode 7 — the EMITTER's own frame, with a fixed axis permutation: the columns
/// are `(q*Y, q*Z, q*X)`, so an unrotated emitter gives a quad standing in the
/// world YZ plane and facing +X. No camera and no velocity anywhere in it; this
/// is the mode for a decal or a panel that belongs to the thing it is on.
///
/// The engine builds it the long way, rotating the three world basis vectors by
/// the quaternion and reading the result back out through the same
/// branch-on-largest-diagonal routine. Rotating the two we need is the same
/// answer without the round trip.
inline bool FrameEmitter(const Quaternion& q, QuadFrame& out) {
    out.right = q.rotate_vector(kWorldY);
    out.up = q.rotate_vector(kWorldUp);
    return true;
}

/// Modes 9 and 10 — ground-conforming. The emitter frame turned so that its own
/// up lands on the terrain normal, then read out as `(R*X, R*Y, R*Z)`.
///
/// That permutation is the other one, and it is what makes these the ground
/// modes: an identity `R` gives right (1,0,0) and up (0,1,0), a quad lying FLAT
/// — the very basis this build used to hand mode 13, which is vertical. The two
/// were swapped.
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

/// The gated column order, as a quaternion.
///
/// Nine of the fourteen modes reach the same three lines through it. The
/// tail of `Particle_QuaternionFromAxes` @0x71000BD2B0 is
/// `Math_QuaternionFromMatrix(out, worldX, worldY, worldZ, n, c, b)`, and
/// `Math_BuildBasisMatrix` inverts the SOURCE basis before multiplying — the
/// source basis is the world's, so the inverse is the identity and the three
/// destination vectors reach the quaternion as its columns unchanged. That
/// makes the whole call `QuatFromBasis(n, c, b)`.
///
/// Columns `(n, up x n, n x (up x n))`. Against the ungated `(-c, b, n)` that
/// is the CYCLIC SHIFT — `up x n` is `-c` and `n x (up x n)` is `b`, term for
/// term — which is the relabel that keeps the plane and moves which axis is
/// which. For a Z-up, +X-forward model that is a facing: the model's own +X
/// turns onto @p n, its +Y onto the horizontal perpendicular, and its +Z stays
/// up for as long as @p n is horizontal.
///
/// @p n is used as given. Every caller but mode 0 hands it a unit vector
/// (`SelectFrameAxis` normalises); mode 0 hands it a view direction, which
/// already is one.
inline bool GatedFrame(const Vector3f& n, Quaternion& out) {
    const Vector3f c0 = FrameCross(kWorldUp, n);
    const f32 cl = FrameLength(c0);
    if (cl <= kFrameEpsilon)
        return false;
    const Vector3f c = FrameNormalise(c0, cl);
    Vector3f b = FrameCross(n, c);
    const f32 bl = FrameLength(b);
    if (bl > kFrameEpsilon)
        b = FrameNormalise(b, bl);
    out = QuatFromBasis(n, c, b);
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

/// The quaternion `Actor_SpawnFromSno` receives, per render mode.
///
/// Not an analogue of the quad frame — the same function read through its other
/// arm. `ParticleSystem_EmitParticle` @0x71000B1A00 runs the whole orientation
/// switch AT EMIT: one motion step (@0x71000B201C), then
/// `Particle_UpdateGroundNormal`, then `Particle_BuildOrientationBasis` into
/// scratch+0xD8 — and @0x71000B2740 hands those exact four floats to
/// `Actor_SpawnFromSno` as the rotation half of its transform.
///
/// False leaves @p out untouched. Modes 1 and 8 write nothing and a degenerate
/// frame writes nothing either; what stands in both cases is the system's own
/// quaternion, which @0x71000B1EC0 copies from `sys+0x64` into the scratch
/// particle before the switch runs.
///
/// The cyclic shift holds only where both arms are built the same way -- the
/// `Particle_SelectOrientationAxis` family and mode 13. Mode 12's ungated arm
/// is a construction of its own and its gated frame is a quarter turn about
/// the shared normal, and modes 2 and 11 lose the camera outright.
///
/// Of the 4,795 shipped child-actor systems: mode 7 takes 2,143 and mode 1
/// 1,116, so 68% want the emitter quaternion and nothing else. The rest are
/// camera-facing (0 x907, 13 x187), velocity-aligned (12 x275, 2 x142, 3 x11)
/// or system-relative (11 x10, 5 x3, 9 x1). Modes 4, 6, 8 and 10 are never
/// asked for by a child-actor system at all.
inline bool BuildChildOrientation(PrtRenderMode renderMode, const FrameInput& in,
                                  Quaternion& out) {
    using enum PrtRenderMode;
    switch (renderMode) {
    case CameraGated:
        // The one mode with no ungated arm at all. `OrientBillboard` is this
        // same construction with the negation folded in; it keeps its own copy
        // because G-D3P-11 pins that one bit for bit.
        return OrientBillboard(in.camForward, out);
    case AxisStreak:
    case AcrossAxis:
    case AxisUpright:
        return GatedFrame(SelectFrameAxis(in.axis, in.axisUnit), out);
    case AcrossSystemXY:
        return GatedFrame(SelectFrameAxis({in.fromSystem.x, in.fromSystem.y, 0.0f}, in.axisUnit),
                          out);
    case AcrossSystem:
    case SystemStreak:
        // Modes 2 and 11 lose the camera here: `Particle_OrientationBasisHelper`
        // @0x71000BBF10 opens with `if (gated) { Particle_QuaternionFromAxes(...);
        // return; }` and never reaches the cross with the view direction that
        // gives the ungated arm its billboard.
        return GatedFrame(SelectFrameAxis(in.fromSystem, in.axisUnit), out);
    case AcrossCameraXY:
        return GatedFrame(SelectFrameAxis({-in.camForward.x, -in.camForward.y, 0.0f}, in.axisUnit),
                          out);
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
