#include "sc2_kernels_spawn.h"

#include "sc2_emitter_desc.h"

#include "renderer/sc2/sc2_element.h"

#include <algorithm>
#include <cmath>

namespace whiteout::flakes::renderer::particle {

namespace {

namespace bits = whiteout::flakes::renderer::sc2;

// The image's own constants, not recomputed: `two_pi` and `deg_to_rad` are the
// exact floats the shipped code multiplies by (RE §16.5).
using sc2::kDegToRad;
using sc2::kPi;
using sc2::kTwoPi;
/// A spline tangent above this |z| takes the world basis. DISAGREES — F4.
using sc2::kSplineVerticalCos;

} // namespace

namespace {

/// `Rand(-h, +h)`. Every box and plane axis is drawn through this kernel, not
/// through `outer · (rand01 - 0.5)`: the two are algebraically identical and
/// differ in the last bit, and OP4 sees the difference on the plane.
f32 SymRand(sc2::Rng& rng, f32 h) {
    return rng.RangeF(-h, h);
}

/// The radius every round shape shares: a range draw when hollow, otherwise
/// `rand01 · outer` — uniform in r, so the volume is centre-biased. That is
/// the shipped distribution, not a bug to fix.
f32 ShapeRadius(sc2::Rng& rng, const Sc2SpawnPosInputs& in) {
    if (in.cutout)
        return rng.RangeF(in.innerRadius, in.outerRadius);
    return rng.RangeF(0.0f, 1.0f) * in.outerRadius;
}

f32 Component(const Vector3f& v, i32 i) {
    return i == 0 ? v.x : (i == 1 ? v.y : v.z);
}

void SetComponent(Vector3f& v, i32 i, f32 value) {
    (i == 0 ? v.x : (i == 1 ? v.y : v.z)) = value;
}

Vector3f Cross(const Vector3f& a, const Vector3f& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

/// Returns false when the vector has no length, leaving `v` untouched.
bool Normalize(Vector3f& v) {
    const f32 n2 = (v.x * v.x + v.y * v.y) + v.z * v.z;
    if (n2 <= 0.0f)
        return false;
    const f32 inv = 1.0f / std::sqrt(n2);
    v = {v.x * inv, v.y * inv, v.z * inv};
    return true;
}

f32 Bezier3(const Vector3f* p, f32 u, i32 i) {
    const f32 iu = 1.0f - u;
    const f32 b0 = (iu * iu) * iu;
    const f32 b1 = (3.0f * u) * (iu * iu);
    const f32 b2 = (3.0f * (u * u)) * iu;
    const f32 b3 = (u * u) * u;
    return (Component(p[0], i) * b0 + Component(p[1], i) * b1) +
           (Component(p[2], i) * b2 + Component(p[3], i) * b3);
}

f32 Bezier3Deriv(const Vector3f* p, f32 u, i32 i) {
    const f32 iu = 1.0f - u;
    const f32 d0 = -3.0f * (iu * iu);
    const f32 d1 = (3.0f * (iu * iu)) - ((6.0f * u) * iu);
    const f32 d2 = ((6.0f * u) * iu) - (3.0f * (u * u));
    const f32 d3 = 3.0f * (u * u);
    return (Component(p[0], i) * d0 + Component(p[1], i) * d1) +
           (Component(p[2], i) * d2 + Component(p[3], i) * d3);
}

/// The overlay groups are sampled at spawn, and only a type-5 wave touches the
/// generator — so where each one sits in the stream is observable exactly
/// there. OP5's original grid armed one group at a time and could not see the
/// order at all; widened to arm two, it says SPEED is sampled first.
f32 Overlay(sc2::Rng& rng, const Sc2Overlay& o, f32 time, f32 phase) {
    if (o.type == 0)
        return 0.0f;
    return sc2::SampleWave(o.type, o.frequency * time + phase, o.amplitude, &rng);
}

} // namespace

Vector3f Sc2SampleSpawnPosition(sc2::Rng& rng, const Sc2SpawnPosInputs& in, Vector3f* normal) {
    switch (in.shape) {
    case Sc2SpawnShape::Point:
    default:
        // Draws NOTHING — the generator must come back untouched, which is
        // half of what OP4's shape-0 vectors are for.
        return {0.0f, 0.0f, 0.0f};

    case Sc2SpawnShape::Plane: {
        // `elemScale` is read one index over: outer.x pairs with scale.y and
        // outer.y with scale.z. Measured, not inferred — the "scaled" extents
        // vector is the only one that can tell the three apart.
        const f32 x = SymRand(rng, 0.5f * (in.shapeOuter.x * in.elemScale.y));
        const f32 y = SymRand(rng, 0.5f * (in.shapeOuter.y * in.elemScale.z));
        return {x, y, 0.0f};
    }

    case Sc2SpawnShape::Sphere: {
        const f32 r = ShapeRadius(rng, in);
        const f32 w = rng.RangeF(-1.0f, 1.0f);
        const f32 az = rng.RangeF(0.0f, 1.0f) * kTwoPi;
        // The recorder read an `asinf(fabs(w))` elevation off the disassembly.
        // It is not observable: the ring lane cannot separate `cos(asin|w|)`
        // from `sqrt(1-w²)`, and the z lane says `r·w` outright — bit-exact on
        // all 96 vectors, where the asin route misses 8 by an ulp. So the
        // measurable form is transcribed and the asin is left as a note.
        const f32 ring = r * std::sqrt(1.0f - w * w);
        // x takes SIN and y takes COS — the swap against the cylinder below,
        // which is the same azimuth applied the other way round.
        return {ring * std::sin(az), ring * std::cos(az), r * w};
    }

    case Sc2SpawnShape::Box: {
        // The axis pick is drawn FIRST, then the three axes in index order —
        // the slab axis is not drawn last. With cube extents every ordering
        // gives the same numbers, so only the "shell" vector separates them.
        Vector3f p{0.0f, 0.0f, 0.0f};
        const i32 k = in.cutout ? static_cast<i32>(rng.RangeInt(0, 3)) : -1;
        for (i32 a = 0; a < 3; ++a) {
            if (a != k) {
                SetComponent(p, a, SymRand(rng, Component(in.shapeOuter, a) * 0.5f));
                continue;
            }
            const f32 span = (Component(in.shapeOuter, a) - Component(in.shapeInner, a)) * 0.5f;
            const f32 v = SymRand(rng, span);
            const f32 half = Component(in.shapeInner, a) * 0.5f;
            SetComponent(p, a, v >= 0.0f ? v + half : v - half);
        }
        return p;
    }

    case Sc2SpawnShape::Cylinder:
    case Sc2SpawnShape::Disc: {
        const f32 r = ShapeRadius(rng, in);
        // The cylinder draws its HEIGHT before its angle. The disc has no
        // height and so takes the angle one slot earlier — the two shapes do
        // NOT share a draw stream, and treating the disc as "a cylinder with
        // z = 0" gives it the cylinder's height draw as its angle.
        const f32 z = in.shape == Sc2SpawnShape::Cylinder
                          ? in.shapeOuter.z * (rng.RangeF(0.0f, 1.0f) - 0.5f)
                          : 0.0f;
        const f32 th = rng.RangeF(0.0f, 1.0f) * kTwoPi;
        return {r * std::cos(th), r * std::sin(th), z};
    }

    case Sc2SpawnShape::Spline: {
        const i32 nSeg = static_cast<i32>(in.spline.size()) / 4;
        if (nSeg <= 0)
            return {0.0f, 0.0f, 0.0f};
        const f32 t = rng.RangeF(in.splineLowerBound, in.splineUpperBound);
        const i32 seg = (std::min)(nSeg - 1, static_cast<i32>(static_cast<f32>(nSeg) * t));
        const f32 u = (t - static_cast<f32>(seg) / static_cast<f32>(nSeg)) *
                      static_cast<f32>(nSeg);
        const Vector3f* cp = in.spline.data() + 4 * seg;
        const Vector3f p{Bezier3(cp, u, 0), Bezier3(cp, u, 1), Bezier3(cp, u, 2)};

        Vector3f tangent{Bezier3Deriv(cp, u, 0), Bezier3Deriv(cp, u, 1),
                         Bezier3Deriv(cp, u, 2)};
        Vector3f n1{1.0f, 0.0f, 0.0f};
        Vector3f n2{0.0f, 1.0f, 0.0f};
        // A vertical tangent has no well-defined frame about world Z, so the
        // world basis stands in. The threshold is the shipped constant, not a
        // round number: a segment at 0.9989 still builds its own frame.
        if (Normalize(tangent) && std::fabs(tangent.z) <= kSplineVerticalCos) {
            n1 = Cross(tangent, Vector3f{0.0f, 0.0f, 1.0f});
            Normalize(n1);
            n2 = Cross(n1, tangent);
            Normalize(n2);
        }
        const f32 r = ShapeRadius(rng, in);
        const f32 az = rng.RangeF(0.0f, 1.0f) * kTwoPi;
        const f32 ca = std::cos(az);
        const f32 sa = std::sin(az);
        return {p.x + ((r * ca) * n1.x + (r * sa) * n2.x),
                p.y + ((r * ca) * n1.y + (r * sa) * n2.y),
                p.z + ((r * ca) * n1.z + (r * sa) * n2.z)};
    }

    case Sc2SpawnShape::Mesh: {
        if (in.mesh == nullptr)
            return {0.0f, 0.0f, 0.0f};
        const Sc2MeshSample s = Sc2SampleMeshSurface(rng, *in.mesh);
        if (normal != nullptr)
            *normal = s.normal;
        return s.position;
    }
    }
}

/// Below this much horizontal speed a flattened velocity keeps no direction.
constexpr f32 kFlattenMinXY = 1e-5f;

Vector3f Sc2SampleSpawnVelocity(sc2::Rng& rng, const Sc2SpawnVelInputs& in) {
    // SPEED FIRST. Only a type-5 overlay draws, so this ordering is invisible
    // until two of them are armed at once; OP5's widened grid is what pins it.
    const f32 speedOverlay = Overlay(rng, in.speedOverlay, in.variationTime, in.variationPhase);
    const f32 yaw = in.spawnYaw +
                    Overlay(rng, in.yawOverlay, in.variationTime, in.variationPhase);
    const f32 pitch = in.spawnPitch +
                      Overlay(rng, in.pitchOverlay, in.variationTime, in.variationPhase);
    const f32 horizontal =
        in.spawnHorizontal +
        Overlay(rng, in.horizontalOverlay, in.variationTime, in.variationPhase);
    const f32 vertical =
        in.spawnVertical + Overlay(rng, in.verticalOverlay, in.variationTime, in.variationPhase);

    Vector3f dir{0.0f, 0.0f, 0.0f};
    f32 speed = 0.0f;

    const auto drawSpeed = [&]() {
        return in.speedIsEndpoint ? rng.RangeF(in.speed, in.speedRandom)
                                  : in.speed + speedOverlay;
    };

    switch (static_cast<Sc2VelocityType>(in.velocityType)) {
    case Sc2VelocityType::Cone: {
        // The overlays are added in DEGREES, before the conversion — an
        // amplitude of 1.5 is 1.5°, not 1.5 rad. Yaw never touches x, which is
        // how the group-0/group-1 swap is visible at all.
        const f32 a = yaw * kDegToRad;
        const f32 b = pitch * kDegToRad;
        const f32 h = rng.RangeF(kPi - horizontal, kPi + horizontal);
        const f32 v = rng.RangeF(-vertical, vertical);
        const f32 cv = std::cos(v);
        const f32 sv = std::sin(v);
        const f32 chb = std::cos(h - b);
        const f32 shb = std::sin(h - b);
        const f32 sa = std::sin(a);
        const f32 ca = std::cos(a);
        dir = {cv * shb, (sa * cv) * chb + ca * sv, sa * sv - (ca * cv) * chb};
        speed = drawSpeed();
        break;
    }
    case Sc2VelocityType::Radial:
        dir = in.position;
        if (!Normalize(dir))
            dir = {0.0f, 0.0f, 0.0f};
        speed = drawSpeed();
        break;
    case Sc2VelocityType::Axis:
        dir = {0.0f, 0.0f, in.position.z >= 0.0f ? 1.0f : -1.0f};
        speed = drawSpeed();
        break;
    case Sc2VelocityType::Random: {
        // Type 3 draws its SPEED BEFORE its direction, and only when the
        // endpoint flag is set — so the w/azimuth pair sits at a different
        // point in the stream depending on a flag about magnitude. The cone
        // above does the opposite. Both orders are measured.
        speed = drawSpeed();
        const f32 w = rng.RangeF(-1.0f, 1.0f);
        const f32 az = rng.RangeF(0.0f, 1.0f) * kTwoPi;
        const f32 ring = std::sqrt(1.0f - w * w);
        dir = {ring * std::sin(az), ring * std::cos(az), w};
        break;
    }
    case Sc2VelocityType::MeshNormal:
    default:
        dir = in.normal;
        speed = drawSpeed();
        break;
    }

    Vector3f out{dir.x * speed, dir.y * speed, dir.z * speed};

    if (in.flattenXY) {
        const f32 mag = std::sqrt((out.x * out.x + out.y * out.y) + out.z * out.z);
        const f32 xy = std::sqrt(out.x * out.x + out.y * out.y);
        // Below the floor there is no direction to keep, so z is dropped and
        // the (tiny) xy is left as it is rather than blown up by mag/xy.
        if (xy > kFlattenMinXY) {
            const f32 s = mag / xy;
            out = {out.x * s, out.y * s, 0.0f};
        } else {
            out.z = 0.0f;
        }
    }
    return out;
}


namespace {

using sc2::kMidTimeCollapse;

/// A random colour lerp's step count: `t` is drawn from `[0, 256)` and the
/// lerp shifts right by 8.
constexpr u32 kColorLerpSteps = 256u;
constexpr f32 kByteMax = 255.0f;

struct Bgra {
    i32 a, r, g, b; // as a u32 the alpha is the HIGH byte
};

Bgra Unpack(u32 v) {
    return {static_cast<i32>((v >> 24) & 0xFFu), static_cast<i32>((v >> 16) & 0xFFu),
            static_cast<i32>((v >> 8) & 0xFFu), static_cast<i32>(v & 0xFFu)};
}

u32 Pack(const Bgra& c) {
    return (static_cast<u32>(c.a & 0xFF) << 24) | (static_cast<u32>(c.r & 0xFF) << 16) |
           (static_cast<u32>(c.g & 0xFF) << 8) | static_cast<u32>(c.b & 0xFF);
}

/// `base + ((t · (random − base)) >> 8)` — an INTEGER lerp toward the random
/// endpoint with an 8-bit shift, not a float lerp rounded afterwards.
i32 LerpChannel(i32 base, i32 random, i32 t) {
    return base + (((t * (random - base)) >> 8));
}

} // namespace

std::array<u32, 3> Sc2SampleColor(sc2::Rng& rng, const Sc2ColorInputs& in) {
    const f32 overlay =
        Overlay(rng, in.alphaOverlay, in.variationTime, in.variationPhase);

    Bgra nodes[3];
    for (i32 n = 0; n < 3; ++n) {
        nodes[n] = Unpack(in.keys[static_cast<std::size_t>(n)]);
        if (!in.randomEnable)
            continue;
        // ONE t per node, drawn per node — not one t shared by all three.
        const i32 t = static_cast<i32>(rng.RangeInt(0, kColorLerpSteps));
        const Bgra r = Unpack(in.randomKeys[static_cast<std::size_t>(n)]);
        nodes[n] = {LerpChannel(nodes[n].a, r.a, t), LerpChannel(nodes[n].r, r.r, t),
                    LerpChannel(nodes[n].g, r.g, t), LerpChannel(nodes[n].b, r.b, t)};
    }

    // The collapse lives INSIDE the randomise branch, exactly as it does in
    // `SampleParticleRotation`. RE §5.8 states it unconditionally for colour;
    // measured, an emitter with no colour randomisation keeps its mid key
    // however high the mid time is.
    if (in.randomEnable) {
        if (in.colorMidTime > kMidTimeCollapse) {
            nodes[1].r = nodes[2].r;
            nodes[1].g = nodes[2].g;
            nodes[1].b = nodes[2].b;
        }
        if (in.alphaMidTime > kMidTimeCollapse)
            nodes[1].a = nodes[2].a;
    }

    std::array<u32, 3> out{};
    for (i32 n = 0; n < 3; ++n) {
        // Added to the alpha BYTE as a float, clamped as a float, and only
        // then truncated. Truncating the overlay to an integer first turns a
        // −0.91 into 0 and leaves alpha untouched where retail drops it by one.
        f32 a = static_cast<f32>(nodes[n].a) + overlay;
        a = (std::max)(0.0f, (std::min)(kByteMax, a));
        Bgra c = nodes[n];
        c.a = static_cast<i32>(a);
        out[static_cast<std::size_t>(n)] = Pack(c);
    }
    return out;
}

std::array<f32, 4> Sc2SampleSize(sc2::Rng& rng, const Sc2SizeInputs& in) {
    const f32 overlay = Overlay(rng, in.sizeOverlay, in.variationTime, in.variationPhase);

    std::array<f32, 3> keys = in.keys;
    if (in.randomEnable) {
        // ONE draw lerps all three keys — unlike colour, which draws per node.
        const f32 t = rng.RangeF(0.0f, 1.0f);
        for (std::size_t k = 0; k < 3; ++k)
            keys[k] = keys[k] + t * (in.randomKeys[k] - keys[k]);
    }

    // Half extents, and the overlay arrives already halved: the shipped form is
    // `(key·0.5 + overlay·0.5)·blend`, not `(key + overlay)·0.5·blend`.
    const f32 half = overlay * 0.5f;
    std::array<f32, 4> out{};
    for (std::size_t k = 0; k < 3; ++k)
        out[k] = (keys[k] * 0.5f + half) * in.blend;
    out[3] =
        Sc2InstanceTypeOf(in.instanceType) == Sc2InstanceType::Pinned ? in.instanceDistance : 1.0f;
    return out;
}

std::array<f32, 3> Sc2SampleRotation(sc2::Rng& rng, const Sc2RotationInputs& in) {
    const f32 overlay =
        Overlay(rng, in.rotationOverlay, in.variationTime, in.variationPhase);

    f32 start = in.keys[0];
    f32 mid = in.keys[1];
    f32 end = in.keys[2];
    if (in.randomEnable) {
        // Three draws, and all three happen in BOTH arms — the relative flag
        // changes what is added, never what is drawn.
        start = rng.RangeF(start, in.randomKeys[0]);
        mid = rng.RangeF(mid, in.randomKeys[1]);
        end = rng.RangeF(end, in.randomKeys[2]);
    }

    std::array<f32, 3> out{};
    out[0] = start + overlay;
    if (in.relative) {
        out[1] = mid + out[0];
        out[2] = end + out[1];
    } else {
        out[1] = mid + overlay;
        out[2] = end + overlay;
    }
    // Random-only, and this one the RE already records (§16.6).
    if (in.randomEnable && in.rotationMidTime > kMidTimeCollapse)
        out[1] = out[2];
    return out;
}


namespace {

/// `×256` for size and `×32` for rotation, both a truncating `cvttss2si` of
/// the sampled float. The samplers themselves return floats (OP6); the
/// quantisation is this function's job, not theirs.
using sc2::kOrientQuant;
using sc2::kRotationQuant;
using sc2::kSizeQuant;
using sc2::kStillSpeedSq;

u16 Quant(f32 v) {
    return static_cast<u16>(static_cast<i32>(v));
}

Vector3f Row(const Matrix44f& m, i32 r) {
    return {m.data[r][0], m.data[r][1], m.data[r][2]};
}

/// The largest squared row length of the basis — `maxColumnLenSq` in the
/// disassembly's naming, over the rows as this layout stores them.
f32 MaxRowLenSq(const Matrix44f& m) {
    f32 best = 0.0f;
    for (i32 r = 0; r < 3; ++r) {
        const Vector3f v = Row(m, r);
        best = (std::max)(best, (v.x * v.x + v.y * v.y) + v.z * v.z);
    }
    return best;
}

Matrix44f NormalizedBasis(const Matrix44f& m) {
    Matrix44f n = m;
    for (i32 r = 0; r < 3; ++r) {
        const Vector3f v = Row(m, r);
        const f32 sq = (v.x * v.x + v.y * v.y) + v.z * v.z;
        if (sq <= 0.0f)
            continue;
        const f32 inv = 1.0f / std::sqrt(sq);
        n.data[r][0] = v.x * inv;
        n.data[r][1] = v.y * inv;
        n.data[r][2] = v.z * inv;
    }
    return n;
}

Vector3f BasisMul(const Matrix44f& b, const Vector3f& v) {
    return {(v.x * b.data[0][0] + v.y * b.data[1][0]) + v.z * b.data[2][0],
            (v.x * b.data[0][1] + v.y * b.data[1][1]) + v.z * b.data[2][1],
            (v.x * b.data[0][2] + v.y * b.data[1][2]) + v.z * b.data[2][2]};
}

f32 PackOrient(f32 hi, f32 lo) {
    const i32 h = static_cast<i32>((hi + 1.0f) * kOrientQuant);
    const i32 l = static_cast<i32>((lo + 1.0f) * kOrientQuant);
    return static_cast<f32>((h << 16) | (l & 0xFFFF));
}

} // namespace

void Sc2InitSpawned(sc2::Rng& rng, const Sc2InitInputs& in, Sc2InitState& state,
                    std::span<Sc2SpawnedElement> out) {
    // The size sampler's scale argument. Exactly 1.0 for slot 0; for a `PARC`
    // slot it is the bone's max row length over the emitter's, and zero when
    // the bone is degenerate.
    f32 sizeBlend = 1.0f;
    if (in.slot > 0 && in.hasBone) {
        const f32 bsq = MaxRowLenSq(in.boneMatrix);
        const f32 wsq = MaxRowLenSq(in.worldMatrix);
        sizeBlend = (bsq <= 0.0f || wsq <= 0.0f) ? 0.0f
                                                 : std::sqrt(bsq) / std::sqrt(wsq);
    }

    // The world arm's BASIS: the emitter's own matrix for slot 0 — and for a
    // Mesh emitter at ANY slot — the PARC bone's rows otherwise.
    const bool emitterBasis =
        in.shape.shape == Sc2SpawnShape::Mesh || in.slot == 0;
    const Matrix44f& basis =
        emitterBasis ? in.worldMatrix : (in.hasBone ? in.boneMatrix : in.worldMatrix);
    const Matrix44f nbasis = NormalizedBasis(basis);

    // Its TRANSLATION is NOT the matrix's: `curPos + spawnPosStep`, and for
    // slot > 0 the bone's translation MINUS the emitter world matrix's, so a
    // PARC copy emits from its bone while simulating in the emitter's frame.
    Vector3f emitPos{state.curPos.x + in.spawnPosStep.x,
                     state.curPos.y + in.spawnPosStep.y,
                     state.curPos.z + in.spawnPosStep.z};
    if (in.slot > 0 && in.hasBone) {
        emitPos = {emitPos.x + in.boneMatrix.data[3][0] - in.worldMatrix.data[3][0],
                   emitPos.y + in.boneMatrix.data[3][1] - in.worldMatrix.data[3][1],
                   emitPos.z + in.boneMatrix.data[3][2] - in.worldMatrix.data[3][2]};
    }

    // instanceType 7 packs two normalised rows into three floats, two u16
    // each: x = right.y<<16 | right.x, y = right.z<<16 | up.x, z = up.y<<16 |
    // up.z.
    const Vector3f right = Row(nbasis, 0);
    const Vector3f up = Row(nbasis, 1);
    const Vector3f fwd = Row(nbasis, 2);
    const Vector3f packed{PackOrient(right.y, right.x), PackOrient(right.z, up.x),
                          PackOrient(up.y, up.z)};

    const Sc2InstanceType type = Sc2InstanceTypeOf(in.instanceType);

    // The local path's transform, built once: bone × inverse(emitter world).
    Matrix44f localXform = Matrix44f::identity();
    if (in.slot > 0 && in.hasBone)
        localXform = in.boneMatrix * Matrix44f::inverse(in.worldMatrix);

    for (std::size_t n = 0; n < out.size(); ++n) {
        Sc2SpawnedElement& e = out[n];
        e = Sc2SpawnedElement{};

        state.emitterTime += in.spawnTimeStep;
        state.curPos = {state.curPos.x + in.spawnPosStep.x,
                        state.curPos.y + in.spawnPosStep.y,
                        state.curPos.z + in.spawnPosStep.z};

        // `CollideTerrain` and `CollideObjects`, each moved one bit up onto the
        // element's collide bits.
        e.flags = static_cast<u16>((in.parFlags * 2) &
                                   (bits::kElemCollideTerrain | bits::kElemCollideObjects));
        if ((in.emitFlagsWord & bits::kEmitNoise) != 0)
            e.noisePhase = rng.RangeF(0.0f, in.noiseCoherence);

        // The normal a velocityType 4 reads is ZEROED first and only the Mesh
        // shape writes it, so type 4 on any other shape has no velocity.
        Vector3f normal{0.0f, 0.0f, 0.0f};
        const bool meshNormal = static_cast<Sc2VelocityType>(in.velocity.velocityType) ==
                                Sc2VelocityType::MeshNormal;
        Vector3f pos = Sc2SampleSpawnPosition(rng, in.shape, meshNormal ? &normal : nullptr);
        Sc2SpawnVelInputs vel = in.velocity;
        vel.position = pos;
        vel.normal = normal;
        Vector3f velocity = Sc2SampleSpawnVelocity(rng, vel);

        const SpawnRequest* req = n < in.requests.size() ? &in.requests[n] : nullptr;
        if (req != nullptr) {
            // Position ADDS, velocity multiplies per axis, orientation is
            // COPIED wholesale rather than combined.
            pos = {pos.x + req->position.x, pos.y + req->position.y,
                   pos.z + req->position.z};
            velocity = {velocity.x * req->velocityScale.x,
                        velocity.y * req->velocityScale.y,
                        velocity.z * req->velocityScale.z};
            e.orientVec = req->orientVec;
        }

        e.birthTime = state.emitterTime;
        const f32 life = Sc2Has(in.additionalFlags, ParticleAdditionalFlag::LifespanRandomize)
                             ? rng.RangeF(in.lifetime, in.lifetimeRandom)
                             : in.lifetime;
        e.deathTime = life + e.birthTime;

        e.colorNodes = Sc2SampleColor(rng, in.color);
        Sc2SizeInputs sz = in.size;
        sz.blend = sizeBlend;
        const auto size = Sc2SampleSize(rng, sz);
        for (std::size_t k = 0; k < 4; ++k)
            e.size[k] = Quant(size[k] * kSizeQuant);
        const auto rot = Sc2SampleRotation(rng, in.rotation);
        for (std::size_t k = 0; k < 3; ++k)
            e.rotation[k] = Quant(rot[k] * kRotationQuant);
        if (Sc2Has(in.rotationFlags, Sc2RotationBit::RandomUvOffset))
            e.flipbookRand = static_cast<u16>(rng.RangeInt(0, 0xFFFF));

        const f32 mass = Sc2Has(in.additionalFlags, ParticleAdditionalFlag::MassRandomize)
                             ? rng.RangeF(in.mass, in.massRandom)
                             : in.mass;
        e.invMass = 1.0f / mass;

        // A request forces the LOCAL path even on a world-space emitter, so a
        // request-born particle is placed in its parent's frame.
        const bool worldArm =
            Sc2Has(in.additionalFlags, ParticleAdditionalFlag::WorldSpace) && req == nullptr;
        if (worldArm) {
            const Vector3f b = BasisMul(basis, pos);
            pos = {b.x + emitPos.x, b.y + emitPos.y, b.z + emitPos.z};
            if ((in.stateFlags & bits::kStateInheritVelocity) != 0) {
                // NORMALISED basis, plus the inherited parent velocity. Both
                // halves hang off this bit: without it the raw basis is used
                // and no parent velocity is inherited at all.
                const Vector3f nv = BasisMul(nbasis, velocity);
                velocity = {nv.x + in.smoothedPos.x * in.inheritVelocityScale,
                            nv.y + in.smoothedPos.y * in.inheritVelocityScale,
                            nv.z + in.smoothedPos.z * in.inheritVelocityScale};
            } else {
                velocity = BasisMul(basis, velocity);
            }
            emitPos = {emitPos.x + in.spawnPosStep.x, emitPos.y + in.spawnPosStep.y,
                       emitPos.z + in.spawnPosStep.z};
            if (type == Sc2InstanceType::EmitterOriented)
                e.orientVec = packed;
            else if (type == Sc2InstanceType::TerrainDirOriented)
                e.orientVec = fwd;
        } else {
            // On the local path a type-7 instance gets the normalised forward
            // row, NOT the packed basis — and a type 6 gets nothing. The two
            // arms disagree about what orientVec even means.
            if (type == Sc2InstanceType::EmitterOriented)
                e.orientVec = fwd;
            if (in.slot > 0 && in.hasBone) {
                const Vector3f p = BasisMul(localXform, pos);
                pos = {p.x + localXform.data[3][0], p.y + localXform.data[3][1],
                       p.z + localXform.data[3][2]};
                velocity = BasisMul(localXform, velocity);
            }
        }

        e.position = pos;
        e.velocity = velocity;

        if (Sc2Has(in.parFlags, ParticleFlag::RandomFlipbookStart) && in.flipbookColumns != 0 &&
            in.flipbookRows != 0) {
            e.flipbookRandStart = rng.RangeF(
                0.0f, static_cast<f32>(static_cast<i32>(in.flipbookColumns) *
                                       static_cast<i32>(in.flipbookRows)));
        }
        e.spawnOrigin = e.position;

        if (Sc2Has(in.parFlags, ParticleFlag::SpawnTrailingParticles) && in.hasChildEmitter1) {
            if (rng.RangeF(0.0f, 1.0f) <= in.trailChance) {
                e.flags |= bits::kElemTrail;
                e.trailAccum = 0.0f;
            }
        }

        // A type-6 instance too slow to point anywhere is killed at birth
        // rather than drawn facing an arbitrary direction.
        if (type == Sc2InstanceType::TerrainDirOriented) {
            const Vector3f& v = e.velocity;
            const f32 sq = (v.y * v.y + v.x * v.x) + v.z * v.z;
            if (sq < kStillSpeedSq) {
                e.deathTime = e.birthTime + -1.0f;
                e.velocity = {1.0f, 0.0f, 0.0f};
                e.orientVec = {1.0f, 0.0f, 0.0f};
            }
        }

        const u32 ms = in.nowMs + static_cast<u32>(static_cast<i32>(
                                      (e.deathTime - e.birthTime) * sc2::kMsPerSec));
        state.expireFrameMs = (std::max)(state.expireFrameMs, ms);
    }
}

Sc2SpawnBatchPlan Sc2PlanSpawnBatch(const Sc2SpawnBatchInputs& in) {
    constexpr u32 kFlushAt = 128;
    Sc2SpawnBatchPlan plan;
    u32 alive = in.elementCount;
    u32 pending = 0;
    Sc2SpawnFlush cur;

    for (u32 i = 0; i < in.requests; ++i) {
        if (alive >= in.maxParticles)
            break;
        ++alive;
        ++plan.created;
        if (cur.requests == 0)
            cur.requestBegin = i;
        ++cur.requests;
        // Only this loop tests the threshold.
        if (++pending >= kFlushAt) {
            plan.flushes.push_back(cur);
            cur = Sc2SpawnFlush{};
            pending = 0;
        }
    }
    for (u32 i = 0; i < in.plain; ++i) {
        if (alive >= in.maxParticles)
            break;
        ++alive;
        ++plan.created;
        ++cur.plain;
        ++pending;
    }
    if (pending != 0)
        plan.flushes.push_back(cur);
    plan.requestsConsumed = !plan.flushes.empty();
    return plan;
}

} // namespace whiteout::flakes::renderer::particle
