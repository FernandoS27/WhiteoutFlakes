// ============================================================================
// SC2 particle kernels for X6: the Mesh spawn shape (OP13), the model-particle
// pose (OP14) and the pending-spawn draw (OP14b). The float operation ORDER is
// the binary's, spelled with explicit parentheses. See SC2_PARTICLE_DESIGN.md §16.8.
// ============================================================================

#include "renderer/particle/sc2/particle_stages_sc2.h"

#include "renderer/particle/sc2/sc2_emitter_desc.h"

#include "renderer/sc2/sc2_element_math.h"
#include "renderer/sc2/sc2_newton.h"

#include <cmath>

namespace whiteout::flakes::renderer::particle::sc2 {

namespace {

namespace vs = whiteout::flakes::renderer::sc2::vs;

using Quat = std::array<f32, 4>;

using renderer::sc2::kCodeScale;
using renderer::sc2::kInv255;
constexpr f32 kInv256 = renderer::sc2::kInvSizeQuant;
constexpr f32 kInv32 = renderer::sc2::kInvRotationQuant;
constexpr f32 kPackStep = 1.0f / renderer::sc2::kPackHalf;
constexpr f32 kPackBack = -renderer::sc2::kPackHalf;
/// The pole test's cosine, the spawn spline's too.
constexpr f32 kPole = renderer::sc2::kVerticalCos;
/// A world-space type-7/8 `orientVec` read as one plain direction rather than a
/// packed pair.
constexpr u32 kStatePlainOrient = renderer::sc2::kStatePlainOrient;

/// `1/sqrt` and one Newton step, `(r·−0.5)·((s·r)·r − 3)` — the grouping every
/// normalise in these three routines takes. `rsqtss` seeds 12 bits on real
/// hardware and is correctly rounded under the oracle's emulator, which is why
/// the replays hold these lanes to a relative bound.
f32 RsqNewton(f32 s) {
    return renderer::sc2::NewtonRsqrt(s, 1.0f / std::sqrt(s));
}

/// The same refinement spelled as a LENGTH, `((s·r)·r − 3)·(−0.5·s·r)`, and
/// masked to 0 at exactly zero so a collapsed vector is 0 rather than NaN.
f32 NewtonLength(f32 s) {
    if (s == 0.0f)
        return 0.0f;
    return renderer::sc2::NewtonLength(s, 1.0f / std::sqrt(s));
}

/// `FpClassify(v) <= 0`: zero, subnormal or normal. The fallback rows below
/// are taken for a NaN or infinite reciprocal — a zero-length input.
bool Finite(f32 v) {
    return std::isfinite(v);
}

f32 S16(u16 v) {
    return static_cast<f32>(static_cast<i16>(v));
}

/// `EvalAnimCurve1D`, which OP10 proved is `vs::InterpolateValue` with the
/// reciprocal built by the caller.
f32 Curve1(u32 mode, f32 k0, f32 k1, f32 k2, f32 t, f32 mid, f32 hold) {
    return vs::InterpolateValue(t, k0, k1, k2, mid, 1.0f / mid, hold, static_cast<int>(mode));
}

Vector3f Neg(const Vector3f& v) {
    return {-v.x, -v.y, -v.z};
}

/// `QuaternionFromMatrix3` (`0x100D57A90`): largest-diagonal branch, then a
/// packed `rsqrtps` normalise whose Newton step groups `(r·r)·s`.
Quat QuatFromRows(const Vector3f& a2, const Vector3f& a3, const Vector3f& a4) {
    const f32 v4 = a2.x;
    const f32 v5 = a2.x + 1.0f;
    const f32 v6 = a3.y;
    const f32 v7 = a4.z;
    const f32 v8 = (v6 + v7) + v5;
    Quat q;
    if (v8 > 1.0f) {
        q = {a3.z - a4.y, a4.x - a2.z, a2.y - a3.x, v8};
    } else if (v4 <= v6 || v4 <= v7) {
        const f32 v15 = 1.0f - v4;
        if (v6 <= v7)
            q = {a2.z + a4.x, a4.y + a3.z, (v15 - v6) + v7, a2.y - a3.x};
        else
            q = {a3.x + a2.y, (v15 + v6) - v7, a4.y + a3.z, a4.x - a2.z};
    } else {
        q = {(v5 - v6) - v7, a3.x + a2.y, a2.z + a4.x, a3.z - a4.y};
    }
    const f32 s = ((q[0] * q[0]) + (q[1] * q[1])) + ((q[2] * q[2]) + (q[3] * q[3]));
    const f32 r = 1.0f / std::sqrt(s);
    const f32 k = ((r * r) * s) + -3.0f;
    return {((q[0] * -0.5f) * r) * k, ((q[1] * -0.5f) * r) * k, ((q[2] * -0.5f) * r) * k,
            ((q[3] * -0.5f) * r) * k};
}

/// `QuaternionFromAxisAngle`: `sincosf(angle · 0.5)`.
Quat AxisAngle(const Vector3f& axis, f32 angle) {
    const f32 h = angle * 0.5f;
    const f32 s = std::sin(h);
    const f32 c = std::cos(h);
    return {axis.x * s, axis.y * s, axis.z * s, c};
}

/// The spin: basis `b` then the axis-angle `a`, as the pose's own inline
/// product groups it.
Quat MulBasisAxis(const Quat& b, const Quat& a) {
    const f32 w = ((b[3] * a[3]) - (b[0] * a[0])) - ((b[1] * a[1]) + (b[2] * a[2]));
    const f32 x = (((a[0] * b[3]) + (b[0] * a[3])) - (a[2] * b[1])) + (b[2] * a[1]);
    const f32 y = (((a[1] * b[3]) + (b[1] * a[3])) + (a[2] * b[0])) - (b[2] * a[0]);
    const f32 z = ((a[2] * b[3]) + (b[2] * a[3])) + ((b[1] * a[0]) - (a[1] * b[0]));
    return {x, y, z, w};
}

/// The trailing preset product, grouped differently from the spin's.
Quat MulPreset(const Quat& q, const Quat& p) {
    const f32 w = ((q[3] * p[3]) - (q[0] * p[0])) - ((q[1] * p[1]) + (q[2] * p[2]));
    const f32 x = ((q[1] * p[2]) + ((q[0] * p[3]) + (q[3] * p[0]))) - (q[2] * p[1]);
    const f32 y = ((q[2] * p[0]) + (q[1] * p[3])) + ((q[3] * p[1]) - (q[0] * p[2]));
    const f32 z = (((q[0] * p[1]) + (q[3] * p[2])) - (q[1] * p[0])) + (q[2] * p[3]);
    return {x, y, z, w};
}

/// A camera row normalised, or its fallback when the reciprocal is not finite.
Vector3f NormalisedOr(const Vector3f& v, const Vector3f& fallback) {
    const f32 s = ((v.z * v.z) + (v.x * v.x)) + (v.y * v.y);
    const f32 r = RsqNewton(s);
    return Finite(r) ? Vector3f{v.x * r, v.y * r, v.z * r} : fallback;
}

/// Types 1, 9 and 10 build this bit for bit the same way: the velocity crossed
/// with the camera's view row, the third row the velocity itself.
Quat VelocityCameraBasis(const Vector3f& vel, const Vector3f& c) {
    const f32 s = (vel.z * vel.z) + ((vel.y * vel.y) + (vel.x * vel.x));
    const f32 r = RsqNewton(s);
    const Vector3f d = Finite(r) ? Vector3f{r * vel.x, vel.y * r, r * vel.z} : Vector3f{0, 0, 1};
    const f32 ax = (c.y * d.z) - (d.y * c.z);
    const f32 ay = (c.z * d.x) - (c.x * d.z);
    const f32 az = (c.x * d.y) - (c.y * d.x);
    const f32 s2 = ((ay * ay) + (ax * ax)) + (az * az);
    const f32 r2 = RsqNewton(s2);
    const Vector3f a = Finite(r2) ? Vector3f{ax * r2, ay * r2, az * r2} : Vector3f{1, 0, 0};
    const f32 bx = (d.z * a.y) - (a.z * d.y);
    const f32 by = (d.x * a.z) - (d.z * a.x);
    const f32 bz = (a.x * d.y) - (a.y * d.x);
    const f32 s3 = ((bz * bz) + (bx * bx)) + (by * by);
    const f32 r3 = RsqNewton(s3);
    const Vector3f b = Finite(r3) ? Vector3f{-(r3 * bx), -(r3 * by), -(bz * r3)}
                                  : Vector3f{-0.0f, -1.0f, -0.0f};
    return QuatFromRows(a, b, d);
}

/// Types 2 and 3: the same construction over the velocity or `instanceAngle`,
/// with the `1e-4` on y that is exactly the guard types 5 and 6 lack.
struct DirectionBasis {
    Quat q;
    Vector3f back; ///< the negated direction — the default spin axis
};

DirectionBasis FacingBasis(const Vector3f& src) {
    const f32 x = src.x;
    const f32 y = src.y + renderer::sc2::kFacingYGuard;
    const f32 s = ((src.z * src.z) + (src.x * src.x)) + (y * y);
    const f32 r = RsqNewton(s);
    const f32 xn = x * r;
    const f32 yn = y * r;
    const f32 zn = r * src.z;
    const f32 my = -yn;
    const f32 s2 = (xn * xn) + (yn * yn);
    const f32 r2 = RsqNewton(s2);
    const Vector3f row0{r2 * my, r2 * xn, 0.0f};
    const f32 v91 = zn * (r2 * xn);
    const f32 v92 = (r2 * my) * zn;
    const f32 v93 = ((r2 * xn) * xn) - ((r2 * my) * yn);
    const f32 s3 = ((v92 * v92) + (v91 * v91)) + (v93 * v93);
    const f32 r3 = RsqNewton(s3);
    const Vector3f row2{-(v91 * r3), v92 * r3, r3 * v93};
    const Vector3f row1{-xn, my, -zn};
    return {QuatFromRows(row0, row1, row2), row1};
}

} // namespace

// ---------------------------------------------------------------------------
// OP13
// ---------------------------------------------------------------------------

MeshSample SampleMeshSurface(renderer::sc2::Rng& rng, const MeshSurfaceInputs& in) {
    MeshSample out;
    if (!in.haveAsset || !in.haveVertexDesc || in.position == nullptr)
        return out;

    const u32 count = static_cast<u32>(in.triangles.size());
    const auto faceAt = [&](u32 i) -> u32 { return i < in.faces.size() ? in.faces[i] : 0u; };
    const auto maskAt = [&](u32 v) -> f32 {
        if (in.colorR.empty())
            return 255.0f;
        return static_cast<f32>(v < in.colorR.size() ? in.colorR[v] : u8{255});
    };

    for (;;) {
        ++out.tries;
        // `PickRandomMeshTriangle` refuses a null or empty table BEFORE its
        // draw, so this costs the stream nothing.
        if (count == 0)
            return out;
        const MeshTriangle& tri = in.triangles[rng.RangeInt(0, count)];
        const MeshRegionBase base =
            tri.region < in.regions.size() ? in.regions[tri.region] : MeshRegionBase{};
        const u32 add = base.bias + base.firstVertex;
        const u32 idx[3] = {add + faceAt(tri.firstIndex), add + faceAt(tri.firstIndex + 1),
                            add + faceAt(tri.firstIndex + 2)};

        f32 a = rng.RangeF(0.0f, 1.0f);
        f32 b = rng.RangeF(0.0f, 1.0f);
        if ((b + a) > 1.0f) {
            b = 1.0f - b;
            a = 1.0f - a;
        }
        const f32 w = (1.0f - a) - b;

        const f32 mask = (maskAt(idx[2]) * w) + ((maskAt(idx[1]) * b) + (maskAt(idx[0]) * a));
        const u32 byte = static_cast<u8>(static_cast<i32>(mask));
        const u32 draw = rng.RangeInt(0, 256);
        // The 32nd attempt is TAKEN: the mask biases where a particle lands and
        // never whether it does.
        if (byte < draw && out.tries < 32)
            continue;

        const Vector3f p0 = in.position(in.ctx, idx[0]);
        const Vector3f p1 = in.position(in.ctx, idx[1]);
        const Vector3f p2 = in.position(in.ctx, idx[2]);
        out.position = {(p2.x * w) + ((p1.x * b) + (p0.x * a)),
                        (p2.y * w) + ((p1.y * b) + (p0.y * a)),
                        (w * p2.z) + ((b * p1.z) + (a * p0.z))};

        const f32 e1x = p1.x - p0.x;
        const f32 e2x = p2.x - p0.x;
        const f32 e1y = p1.y - p0.y;
        const f32 e2y = p2.y - p0.y;
        const f32 e1z = p1.z - p0.z;
        const f32 e2z = p2.z - p0.z;
        const f32 nx = (e2z * e1y) - (e2y * e1z);
        const f32 ny = (e1z * e2x) - (e2z * e1x);
        const f32 nz = (e2y * e1x) - (e2x * e1y);
        const f32 sq = ((nz * nz) + (nx * nx)) + (ny * ny);
        if (sq == 0.0f) {
            out.normal = {0.0f, 0.0f, 1.0f};
        } else {
            const f32 r = RsqNewton(sq);
            out.normal = {nx * r, ny * r, r * nz};
        }
        out.hit = true;
        return out;
    }
}

// ---------------------------------------------------------------------------
// OP14
// ---------------------------------------------------------------------------

// Rebuilt by the gate with the image's own `Math_MatrixFromYawPitchRoll`, and
// replayed against the golden's `presets` row.
const std::array<std::array<f32, 4>, 7> kModelOrientPresets = {{
    {0.0f, 0.7071067690849304f, 0.0f, 0.7071067094802856f},
    {3.0908619663705394e-08f, 0.7071067690849304f, -0.7071067690849304f, 3.0908619663705394e-08f},
    {0.0f, 0.7071067690849304f, 0.0f, 0.7071067094802856f},
    {0.0f, 1.0f, 0.0f, 4.371138828673793e-08f},
    {0.0f, 1.0f, 0.0f, 4.371138828673793e-08f},
    {3.0908619663705394e-08f, 0.7071067690849304f, -0.7071067690849304f, 3.0908619663705394e-08f},
    {0.7071067690849304f, -3.0908619663705394e-08f, 0.7071067690849304f, -3.0908619663705394e-08f},
}};

std::array<f32, 4> EvalCurve2D(u32 mode, const std::array<f32, 4>& k0,
                                  const std::array<f32, 4>& k1,
                                  const std::array<f32, 4>& k2, f32 t, f32 mid, f32 hold) {
    const f32 inv = 1.0f / mid;
    std::array<f32, 4> out = k0;
    // `(a − b)·f + b` over four lanes.
    const auto lerp = [&](const std::array<f32, 4>& lo, const std::array<f32, 4>& hi, f32 f) {
        for (usize i = 0; i < 4; ++i)
            out[i] = ((hi[i] - lo[i]) * f) + lo[i];
    };
    // `(u·u)·a`, then `+ ((u+u)·t)·b`, then `+ (t·t)·c`.
    const auto bezier = [&](const std::array<f32, 4>& a, const std::array<f32, 4>& b,
                            const std::array<f32, 4>& c, f32 u, f32 tt) {
        const f32 uu = u * u;
        const f32 ut = (u + u) * tt;
        const f32 t2 = tt * tt;
        for (usize i = 0; i < 4; ++i) {
            const f32 first = uu * a[i];
            const f32 second = (ut * b[i]) + first;
            out[i] = (t2 * c[i]) + second;
        }
    };
    switch (mode) {
    case 0:
        if (t < mid)
            lerp(k0, k1, inv * t);
        else
            lerp(k1, k2, (t - mid) / (1.0f - mid));
        break;
    case 1:
        if (t >= mid) {
            const f32 s = (t - mid) / (1.0f - mid);
            lerp(k1, k2, ((s * -2.0f) + 3.0f) * (s * s));
        } else {
            const f32 g = inv * t;
            lerp(k0, k1, ((g * -2.0f) + 3.0f) * (g * g));
        }
        break;
    case 2:
        bezier(k0, k1, k2, 1.0f - t, t);
        break;
    case 3:
        if ((mid - hold) <= t) {
            const f32 top = hold + mid;
            if (top > t)
                out = k1;
            else
                lerp(k1, k2, (t - top) / (1.0f - top));
        } else {
            lerp(k0, k1, t / (mid - hold));
        }
        break;
    case 4:
        if ((mid - hold) <= t) {
            const f32 raw = (t - (hold + mid)) / (1.0f - (hold + mid));
            const f32 upper = std::fmin(1.0f, raw);
            const f32 tt = raw < 0.0f ? 0.0f : upper;
            bezier(k1, k1, k2, 1.0f - tt, tt);
        } else {
            const f32 tt = t / (mid - hold);
            bezier(k0, k1, k1, 1.0f - tt, tt);
        }
        break;
    default:
        break;
    }
    return out;
}

std::array<Vector3f, 3> QuatRows(const std::array<f32, 4>& q) {
    const f32 x = q[0], y = q[1], z = q[2], w = q[3];
    const f32 xx = x * x, yy = y * y, zz = z * z;
    const f32 xy = x * y, xz = x * z, yz = y * z;
    const f32 wx = w * x, wy = w * y, wz = w * z;
    return {Vector3f{1.0f - 2.0f * (yy + zz), 2.0f * (xy + wz), 2.0f * (xz - wy)},
            Vector3f{2.0f * (xy - wz), 1.0f - 2.0f * (xx + zz), 2.0f * (yz + wx)},
            Vector3f{2.0f * (xz + wy), 2.0f * (yz - wx), 1.0f - 2.0f * (xx + yy)}};
}

namespace {

/// What one instance type's arm of the orientation tier builds: the basis,
/// and whether — and about which axis — the rotation curve's angle spins it.
struct PoseBasis {
    Quat q{0.0f, 0.0f, 0.0f, 1.0f};
    bool spin = false;
    Vector3f axis{0, 0, 0};
};

/// Type 0: the camera's own rows, spun about the view direction.
PoseBasis BillboardPose(const ModelPoseInputs& in) {
    const Vector3f fwd = NormalisedOr(in.camera[1], {0.0f, -1.0f, 0.0f});
    const Vector3f right = NormalisedOr(in.camera[0], {1.0f, 0.0f, 0.0f});
    const Vector3f up = NormalisedOr(in.camera[2], {0.0f, 0.0f, 1.0f});
    return {QuatFromRows(right, fwd, up), true, Neg(fwd)};
}

/// Types 1 and 10: the velocity-and-camera basis, stretched along z by the
/// tail, and a Trail slid back along its velocity by that same length.
Quat TailPose(const ModelPoseInputs& in, InstanceType type, Vector3f& pos,
              ModelPose& out) {
    const Vector3f& vel = in.velocity;
    const Quat q = VelocityCameraBasis(vel, in.camera[1]);
    const f32 speed = NewtonLength((vel.z * vel.z) + ((vel.y * vel.y) + (vel.x * vel.x)));
    f32 stretch = in.tailLength * speed;
    if (!Has(in.parFlags, ParticleFlag::FixTailLengthOnCreation))
        stretch = std::fmax(stretch, in.tailLength);
    const f32 z = stretch * out.scale.z;
    out.scale.z = z;
    if (type == InstanceType::Trail) {
        Vector3f dir = vel;
        if (speed > 0.0f) {
            const f32 inv = 1.0f / speed;
            dir = {dir.x * inv, dir.y * inv, dir.z * inv};
        }
        pos = {pos.x - (dir.x * z), pos.y - (dir.y * z), pos.z - (dir.z * z)};
    }
    return q;
}

/// Types 2 and 3: the facing basis over the velocity or `instanceAngle`.
PoseBasis FacingPose(const ModelPoseInputs& in, InstanceType type) {
    const DirectionBasis basis =
        FacingBasis(type == InstanceType::FaceTravelDir ? in.velocity : in.instanceAngle);
    // Under `RotationBit::RandomDirection` a type 3 spins about the
    // element's own random direction instead of its forward.
    const bool randomAxis = type == InstanceType::FaceWorldDir &&
                            Has(in.rotationFlags, RotationBit::RandomDirection);
    return {basis.q, true, randomAxis ? in.randomDirection : basis.back};
}

/// Type 4: three rows crossed off the camera and the negated instance angle,
/// each normalise falling back to a fixed axis when it comes out non-finite.
PoseBasis SingleAxisPose(const ModelPoseInputs& in) {
    const Vector3f& cam = in.camera[1];
    const Vector3f& ia = in.instanceAngle;
    const f32 ex = -ia.x;
    const f32 ey = -ia.y;
    const f32 ez = -ia.z;
    const f32 ax = (cam.z * ey) - (cam.y * ez);
    const f32 ay = (cam.x * ez) - (ex * cam.z);
    const f32 az = (cam.y * ex) - (ey * cam.x);
    const f32 s = ((ay * ay) + (ax * ax)) + (az * az);
    const f32 r = RsqNewton(s);
    const Vector3f a = Finite(r) ? Vector3f{ax * r, ay * r, az * r} : Vector3f{1, 0, 0};
    const f32 bx = (a.y * ez) - (ey * a.z);
    const f32 by = (a.z * ex) - (ez * a.x);
    const f32 bz = (a.x * ey) - (ex * a.y);
    const f32 s2 = ((bz * bz) + (bx * bx)) + (by * by);
    const f32 r2 = RsqNewton(s2);
    const Vector3f b = Finite(r2) ? Vector3f{bx * r2, by * r2, bz * r2} : Vector3f{0, 0, -1};
    const f32 cx = (b.z * a.y) - (a.z * b.y);
    const f32 cy = (a.z * b.x) - (b.z * a.x);
    const f32 cz = (a.x * b.y) - (b.x * a.y);
    const f32 s3 = ((cz * cz) + (cx * cx)) + (cy * cy);
    const f32 r3 = RsqNewton(s3);
    const Vector3f c = Finite(r3) ? Vector3f{cx * r3, cy * r3, cz * r3} : Vector3f{0, 1, 0};
    return {QuatFromRows(a, b, c), true, Neg(b)};
}

/// Type 5: the instance angle projected onto the terrain plane.
Quat TerrainPose(const ModelPoseInputs& in, f32 angle) {
    const Vector3f& ia = in.instanceAngle;
    // No guard on this normalise: a zero `instanceAngle` is a NaN pose.
    const f32 s = ((ia.z * ia.z) + (ia.x * ia.x)) + (ia.y * ia.y);
    const f32 r = RsqNewton(s);
    const Vector3f u{r * ia.x, r * ia.y, r * ia.z};
    const Vector3f n = in.haveTerrain ? in.terrainNormal : Vector3f{0, 0, 1};
    const f32 dp = ((n.z * u.z) + (n.x * u.x)) + (u.y * n.y);
    const f32 px = u.x - (dp * n.x);
    const f32 py = u.y - (dp * n.y);
    const f32 pz = u.z - (dp * n.z);
    const f32 s2 = ((pz * pz) + (px * px)) + (py * py);
    Vector3f p{1, 0, 0};
    if (s2 >= renderer::sc2::kTerrainProjectMinSq) {
        const f32 r2 = RsqNewton(s2);
        p = {px * r2, py * r2, r2 * pz};
    }
    const f32 sx = (p.y * n.z) - (p.z * n.y);
    const f32 sy = (p.z * n.x) - (p.x * n.z);
    const f32 sz = (p.x * n.y) - (p.y * n.x);
    // The spin is applied to the ROWS, about the negated normal, rather
    // than as a trailing product.
    const Quat qa = AxisAngle(Neg(n), angle);
    const f32 x = qa[0], y = qa[1], z = qa[2], w = qa[3];
    const f32 v217 = w * (x + x);
    const f32 v218 = (z + z) * w;
    const f32 v219 = w * (y + y);
    const f32 v220 = (y + y) * x;
    const f32 v221 = x * (z + z);
    const f32 v222 = (y + y) * y;
    const f32 v223 = y * (z + z);
    const f32 v224 = (z + z) * z;
    const f32 v225 = v218 + v220;
    const f32 v226 = v220 - v218;
    const f32 oneXX = 1.0f - ((x + x) * x);
    const f32 v227 = (1.0f - v222) - v224;
    const f32 v228 = oneXX - v224;
    const f32 v229 = v221 - v219;
    const f32 v230 = v221 + v219;
    const f32 v231 = v223 + v217;
    const f32 v232 = v223 - v217;
    const f32 v215 = oneXX - v222;
    const Vector3f rp{(p.z * v230) + ((v226 * p.y) + (v227 * p.x)),
                      (p.z * v232) + ((v228 * p.y) + (v225 * p.x)),
                      (p.z * v215) + ((p.y * v231) + (p.x * v229))};
    const Vector3f rs{(v230 * sz) + ((v226 * sy) + (v227 * sx)),
                      (v232 * sz) + ((v228 * sy) + (v225 * sx)),
                      (v215 * sz) + ((v231 * sy) + (v229 * sx))};
    return QuatFromRows(rs, Neg(u), rp);
}

/// Type 6: the velocity crossed with the terrain normal.
Quat TerrainDirPose(const ModelPoseInputs& in, f32 angle) {
    const Vector3f& vel = in.velocity;
    const Vector3f& ia = in.instanceAngle;
    const f32 s = ((ia.z * ia.z) + (ia.x * ia.x)) + (ia.y * ia.y);
    const f32 r0 = 1.0f / std::sqrt(s);
    const f32 rI = renderer::sc2::NewtonRsqrt(s, r0);
    const Vector3f n = in.haveTerrain ? in.terrainNormal : Vector3f{0, 0, 1};
    const f32 ax = (vel.y * n.z) - (vel.z * n.y);
    const f32 ay = (vel.z * n.x) - (vel.x * n.z);
    const f32 az = (vel.x * n.y) - (vel.y * n.x);
    const f32 s2 = ((az * az) + (ax * ax)) + (ay * ay);
    const f32 r2 = RsqNewton(s2);
    const Vector3f a = Finite(r2) ? Vector3f{ax * r2, ay * r2, az * r2} : Vector3f{1, 0, 0};
    // Rotated about the negated normal by an angle type 6 never evaluates,
    // so this is the identity — kept in the image's groupings anyway.
    const Quat qa = AxisAngle(Neg(n), angle);
    const f32 x = qa[0], y = qa[1], z = qa[2], w = qa[3];
    const f32 x2 = x + x, y2 = y + y, z2 = z + z;
    const f32 l0 = ((a.z * ((1.0f - (x2 * x)) - (y2 * y))) + (a.y * ((y * z2) + (x2 * w)))) +
                   (a.x * ((z2 * x) - (y2 * w)));
    const f32 l1 = ((a.z * ((z2 * x) + (y2 * w))) + (a.y * ((x * y2) - (z2 * w)))) +
                   (a.x * ((1.0f - (y2 * y)) - (z2 * z)));
    const f32 rz = (((y * z2) - (x2 * w)) * a.z) +
                   ((((1.0f - (x2 * x)) - (z2 * z)) * a.y) + (((z2 * w) + (x * y2)) * a.x));
    const Vector3f row0{l1, rz, l0};
    const f32 bx = (n.y * row0.z) - (row0.y * n.z);
    const f32 by = (n.z * row0.x) - (row0.z * n.x);
    const f32 bz = (row0.y * n.x) - (n.y * row0.x);
    const f32 s3 = ((bz * bz) + (bx * bx)) + (by * by);
    const f32 r3 = RsqNewton(s3);
    const Vector3f b = Finite(r3) ? Vector3f{bx * r3, by * r3, bz * r3} : Vector3f{0, 1, 0};
    return QuatFromRows(row0, Vector3f{-(ia.x * rI), -(ia.y * rI), -(ia.z * rI)}, b);
}

/// The three rows a type 7/8 basis is built from.
struct OrientRows {
    Vector3f a{0, 0, 0};
    Vector3f b{0, 0, 0};
    Vector3f c{0, 0, 0};
};

/// World space, the packed `orientVec` spawn wrote.
OrientRows PackedOrientRows(const Vector3f& ov) {
    OrientRows rows;
    // Two directions packed as six byte codes in three floats,
    // `lo + 65536·hi`, interleaved (x.lo, x.hi, y.hi) and
    // (y.lo, z.hi, z.lo). The hi half truncates toward zero.
    const auto split = [](f32 v, f32& hi, f32& lo) {
        const f32 h = kPackStep * v;
        hi = h >= 0.0f ? std::floor(h) : -std::floor(-h);
        lo = v + (kPackBack * hi);
    };
    f32 hx, lx, hy, ly, hz, lz;
    split(ov.x, hx, lx);
    split(ov.y, hy, ly);
    split(ov.z, hz, lz);
    const Vector3f av{(lx * kCodeScale) + -1.0f, (hx * kCodeScale) + -1.0f,
                      (hy * kCodeScale) + -1.0f};
    const f32 s = ((av.z * av.z) + (av.y * av.y)) + (av.x * av.x);
    const f32 r = RsqNewton(s);
    rows.a = {av.x * r, av.y * r, r * av.z};
    const f32 bz0 = (hz * kCodeScale) + -1.0f;
    const f32 bz1 = (lz * kCodeScale) + -1.0f;
    const f32 bx0 = (ly * kCodeScale) + -1.0f;
    const f32 s2 = (bz1 * bz1) + ((bz0 * bz0) + (bx0 * bx0));
    const f32 r2 = RsqNewton(s2);
    rows.b = {bx0 * r2, bz0 * r2, r2 * bz1};
    const f32 v318 = rows.a.x * (r2 * bz1);
    const f32 cx = ((r2 * bz1) * rows.a.y) - ((bz0 * r2) * rows.a.z);
    const f32 cy = (rows.a.z * rows.b.x) - v318;
    const f32 cz = (rows.a.x * rows.b.y) - (rows.b.x * rows.a.y);
    const f32 s3 = (cz * cz) + ((cy * cy) + (cx * cx));
    const f32 r3 = RsqNewton(s3);
    rows.c = {cx * r3, cy * r3, r3 * cz};
    return rows;
}

/// World space under the plain-orient bit: false, and @p rows unused, when
/// the direction is too short to frame.
bool PlainOrientRows(const Vector3f& ov, OrientRows& rows) {
    // One plain direction, with a reference that swaps to a fixed
    // (±1, 0) near either pole. Anything too short to build a
    // frame from leaves the basis at identity and does not spin.
    const f32 s = (ov.z * ov.z) + ((ov.y * ov.y) + (ov.x * ov.x));
    if (s >= renderer::sc2::kPlainOrientMinSq) {
        const f32 r = RsqNewton(s);
        const Vector3f d{ov.x * r, ov.y * r, r * ov.z};
        f32 refX;
        f32 refY;
        if (d.z >= kPole) {
            refX = 1.0f;
            refY = 0.0f;
        } else if (d.z > -kPole) {
            refX = d.y;
            refY = -d.x;
        } else {
            refX = -1.0f;
            refY = 0.0f;
        }
        const f32 s2 = (refY * refY) + (refX * refX);
        if (s2 >= renderer::sc2::kPlainOrientMinSq) {
            const f32 r2 = RsqNewton(s2);
            rows.a = {refX * r2, r2 * refY, 0.0f};
            const f32 v345 = (r2 * refY) * d.z;
            const f32 v346 = (refX * r2) * d.z;
            const f32 v347 = (rows.a.y * d.x) - ((refX * r2) * d.y);
            const f32 s3 = (v347 * v347) + ((v346 * v346) + (v345 * v345));
            if (s3 >= renderer::sc2::kPlainOrientMinSq) {
                const f32 r3 = RsqNewton(s3);
                rows.b = {(-(rows.a.y * d.z)) * r3, v346 * r3, r3 * v347};
                rows.c = d;
                return true;
            }
        }
    }
    return false;
}

/// Local space: rows 0 and 1 of the emitter's own world matrix, and
/// `orientVec` is dead.
OrientRows LocalOrientRows(const std::array<f32, 16>& m, f32 rowLen0, f32 rowLen1) {
    OrientRows rows;
    const f32 r0 = RsqNewton(rowLen0);
    const f32 r1 = RsqNewton(rowLen1);
    rows.a = {m[0] * r0, m[1] * r0, r0 * m[2]};
    rows.b = {m[4] * r1, m[5] * r1, r1 * m[6]};
    const f32 v70 = (m[0] * r0) * (r1 * m[6]);
    const f32 v71 = (m[0] * r0) * (m[5] * r1);
    const f32 cx = ((r1 * m[6]) * (m[1] * r0)) - ((m[5] * r1) * (r0 * m[2]));
    const f32 cy = ((r0 * m[2]) * rows.b.x) - v70;
    const f32 cz = v71 - (rows.b.x * rows.a.y);
    const f32 s3 = (cz * cz) + ((cy * cy) + (cx * cx));
    const f32 r3 = RsqNewton(s3);
    rows.c = {cx * r3, cy * r3, r3 * cz};
    return rows;
}

/// Types 7 and 8.
PoseBasis EmitterOrientedPose(const ModelPoseInputs& in, bool worldSpace, f32 rowLen0,
                              f32 rowLen1) {
    OrientRows rows;
    if (!worldSpace)
        rows = LocalOrientRows(in.world, rowLen0, rowLen1);
    else if ((in.stateFlags & kStatePlainOrient) == 0)
        rows = PackedOrientRows(in.orientVec);
    else if (!PlainOrientRows(in.orientVec, rows))
        return {};
    return {QuatFromRows(rows.a, Neg(rows.c), rows.b), true, Neg(rows.c)};
}

/// Type 9: the velocity-and-camera basis, lengthened along z by how far the
/// particle has drifted from where it spawned.
Quat PinnedPose(const ModelPoseInputs& in, bool worldSpace, const Vector3f& pos,
                ModelPose& out) {
    const std::array<f32, 16>& m = in.world;
    Vector3f o = in.spawnOrigin;
    if (!worldSpace) {
        const Vector3f& p = in.spawnOrigin;
        o = {((m[8] * p.z) + (m[4] * p.y)) + ((m[0] * p.x) + m[12]),
             ((m[9] * p.z) + (m[5] * p.y)) + ((m[1] * p.x) + m[13]),
             ((m[10] * p.z) + (m[6] * p.y)) + ((m[2] * p.x) + m[14])};
    }
    const Quat q = VelocityCameraBasis(in.velocity, in.camera[1]);
    // The drift ADDS to z, in world units the size curve never sees.
    const f32 dz = pos.z - o.z;
    const f32 dy = pos.y - o.y;
    const f32 dx = pos.x - o.x;
    out.scale.z = NewtonLength((dz * dz) + ((dy * dy) + (dx * dx))) + out.scale.z;
    return q;
}

} // namespace

ModelPose ModelParticlePose(const ModelPoseInputs& in) {
    namespace mid = renderer::sc2::MidChannel;
    ModelPose out;
    const std::array<f32, 16>& m = in.world;
    const f32 rowLen0 = (m[2] * m[2]) + ((m[1] * m[1]) + (m[0] * m[0]));
    const f32 rowLen1 = (m[6] * m[6]) + ((m[5] * m[5]) + (m[4] * m[4]));

    // Saturated at both ends by two different idioms: `fminf` for the top and
    // an ANDNOT against `age < 0` for the bottom.
    const f32 age = (in.emitterTime - in.birthTime) / (in.deathTime - in.birthTime);
    const f32 ageTop = std::fmin(1.0f, age);
    const f32 t = age < 0.0f ? 0.0f : ageTop;

    Vector3f pos = in.position;
    const bool worldSpace = Has(in.additionalFlags, ParticleAdditionalFlag::WorldSpace);
    if (!worldSpace) {
        const Vector3f& p = in.position;
        pos = {((p.z * m[8]) + (p.y * m[4])) + ((p.x * m[0]) + m[12]),
               ((p.z * m[9]) + (p.y * m[5])) + ((p.x * m[1]) + m[13]),
               ((p.z * m[10]) + (p.y * m[6])) + ((p.x * m[2]) + m[14])};
    }

    // ---- the scalar tier ----
    const bool elementKeys = Has(in.rotationFlags, RotationBit::ElementKeys);
    f32 size;
    if (!elementKeys) {
        size = Curve1(in.sizeSmoothing, in.sizeKeys[0], in.sizeKeys[1], in.sizeKeys[2], t,
                      in.midTime[mid::Size], in.midHold[mid::Size]);
    } else {
        // The LONGEST world row, which the emitter-cache path never applies.
        const f32 rowLen2 = (m[10] * m[10]) + ((m[9] * m[9]) + (m[8] * m[8]));
        const f32 longest =
            rowLen1 < rowLen0 ? std::fmax(rowLen2, rowLen0) : std::fmax(rowLen2, rowLen1);
        const f32 rowScale = NewtonLength(longest);
        size = rowScale * Curve1(in.sizeSmoothing, S16(in.elementSize[0]) * kInv256,
                                 S16(in.elementSize[1]) * kInv256,
                                 S16(in.elementSize[2]) * kInv256, t, in.midTime[mid::Size],
                                 in.midHold[mid::Size]);
    }
    const f32 uniform = std::fmax(size, renderer::sc2::kModelScaleFloor);
    out.scale = {uniform, uniform, uniform};

    const std::array<u32, 3>& words = elementKeys ? in.elementColors : in.colorKeys;
    std::array<std::array<f32, 4>, 3> keys{};
    for (usize k = 0; k < 3; ++k) {
        const u32 wd = words[k];
        keys[k] = {static_cast<f32>((wd >> 16) & 0xFFu) * kInv255,
                   static_cast<f32>((wd >> 8) & 0xFFu) * kInv255,
                   static_cast<f32>(wd & 0xFFu) * kInv255,
                   static_cast<f32>((wd >> 24) & 0xFFu) * kInv255};
    }
    const auto colour =
        EvalCurve2D(in.colorSmoothing, keys[0], keys[1], keys[2], t, in.midTime[mid::Color],
                       in.midHold[mid::Color]);
    out.tint = {colour[0] * in.modelTint.x, colour[1] * in.modelTint.y, colour[2] * in.modelTint.z};
    const f32 alpha = Curve1(in.colorSmoothing, keys[0][3], keys[1][3], keys[2][3], t,
                             in.midTime[mid::Alpha], in.midHold[mid::Alpha]);
    const f32 alphaTop = std::fmin(1.0f, alpha);
    out.alpha = (alpha < 0.0f ? 0.0f : alphaTop) * in.modelAlpha;

    const InstanceType type = InstanceTypeOf(in.instanceType);
    f32 angle = 0.0f;
    if (type != InstanceType::TerrainDirOriented) {
        if (elementKeys) {
            angle = Curve1(in.rotationSmoothing, S16(in.elementRotation[0]) * kInv32,
                           S16(in.elementRotation[1]) * kInv32,
                           S16(in.elementRotation[2]) * kInv32, t, in.midTime[mid::Rotation],
                           in.midHold[mid::Rotation]);
        } else {
            angle = Curve1(in.rotationSmoothing, in.rotationKeys[0], in.rotationKeys[1],
                           in.rotationKeys[2], t, in.midTime[mid::Rotation],
                           in.midHold[mid::Rotation]);
        }
    }

    // ---- the orientation tier ----
    PoseBasis basis;
    switch (type) {
    case InstanceType::Billboard:
        basis = BillboardPose(in);
        break;
    case InstanceType::Tail:
    case InstanceType::Trail:
        basis.q = TailPose(in, type, pos, out);
        break;
    case InstanceType::FaceTravelDir:
    case InstanceType::FaceWorldDir:
        basis = FacingPose(in, type);
        break;
    case InstanceType::SingleAxis:
        basis = SingleAxisPose(in);
        break;
    case InstanceType::TerrainOriented:
        basis.q = TerrainPose(in, angle);
        break;
    case InstanceType::TerrainDirOriented:
        basis.q = TerrainDirPose(in, angle);
        break;
    case InstanceType::EmitterOriented:
    case InstanceType::PhysicsOriented:
        basis = EmitterOrientedPose(in, worldSpace, rowLen0, rowLen1);
        break;
    case InstanceType::Pinned:
        basis.q = PinnedPose(in, worldSpace, pos, out);
        break;
    default:
        break;
    }

    Quat q = basis.q;
    if (basis.spin)
        q = MulBasisAxis(q, AxisAngle(basis.axis, angle));

    // `SwapYZOnModelParticles`: a 90° yaw about Z, before the preset.
    if (Has(in.parFlags, ParticleFlag::SwapYZOnModelParticles)) {
        constexpr f32 k = 0.7071067690849304f;
        q = {(q[1] + q[0]) * k, (q[1] - q[0]) * k, (q[2] + q[3]) * k, (q[3] - q[2]) * k};
    }

    Quat preset{0.0f, 0.0f, 0.0f, 1.0f};
    if (in.legacyOrient) {
        i32 index = -1;
        switch (type) {
        case InstanceType::FaceTravelDir: index = 0; break;
        case InstanceType::FaceWorldDir:
            index = in.orientVariant == 6 ? 5 : (in.orientVariant != 0 ? 2 : 1);
            break;
        case InstanceType::TerrainOriented: index = 3; break;
        case InstanceType::TerrainDirOriented: index = 4; break;
        case InstanceType::EmitterOriented: index = 6; break;
        default: break;
        }
        if (index >= 0)
            preset = kModelOrientPresets[static_cast<usize>(index)];
    }
    out.rotation = MulPreset(q, preset);
    out.position = pos;
    return out;
}

// ---------------------------------------------------------------------------
// OP14b
// ---------------------------------------------------------------------------

bool PendingSpawnDraw(renderer::sc2::Rng& rng, f32 deathTime, f32 emitterTime, u32 pathCount,
                         bool randomDirection, PendingDraw& out) {
    out = PendingDraw{};
    if (!(deathTime >= emitterTime) || pathCount == 0)
        return false;
    out.pathIndex = rng.RangeInt(0, 0x7FFFFFFFu) % pathCount;
    if (randomDirection) {
        const f32 x = rng.RangeF(-1.0f, 1.0f);
        const f32 y = rng.RangeF(-1.0f, 1.0f);
        const f32 z = rng.RangeF(-1.0f, 1.0f);
        const f32 r = RsqNewton((z * z) + ((y * y) + (x * x)));
        out.hasDirection = true;
        out.direction = {x * r, y * r, r * z};
    }
    return true;
}

} // namespace whiteout::flakes::renderer::particle::sc2
