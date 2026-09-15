#include "renderer/particle/d3/d3_emitter.h"

#include "renderer/particle/d3/d3_emitter_math.h"

#include <cmath>

namespace whiteout::flakes::renderer::particle::d3 {

namespace {

using detail::kAzimuthTwoPi;
using detail::WrapAngle;

constexpr f32 kHalfPi = 1.57079632679489661923f;
/// `Math_AsinFast` @0x7100979640 — the polynomial arcsine the sphere samplers
/// use. Its negative branch returns the angle WRAPPED INTO [0, 6.2832] rather
/// than in [-pi/2, 0], through the same clamp-then-wrap `WrapAngle` reproduces;
/// `-r` differs measurably (G-D3P-01, §30.14).
f32 AsinFast(f32 x) {
    // Abramowitz & Stegun 4.4.45 at the constant pool's FULL precision
    // (@0x7100E3C394..0x7100E3C3A0, pi/2 @0x7100E3BF6C), not the decompile's
    // five-digit prints (§20.1, §30.14).
    const f32 a = std::fmin(std::fabs(x), 1.0f);
    const f32 poly = ((a * -0.0187293f + 0.0742610f) * a - 0.2121144f) * a + 1.5707288f;
    const f32 r = kHalfPi - poly * std::sqrt(1.0f - a);
    return (x < 0.0f) ? WrapAngle(-r) : r;
}

} // namespace

// ---------------------------------------------------------------------------
// Shape samplers. Split out of the emitter so the shape gate can drive them
// directly — the distributions are the part most likely to be got wrong.
// ---------------------------------------------------------------------------

f32 SampleRadiusInAnnulus(MwcRng& rng, f32 inner, f32 thickness) {
    const f32 outer = inner + thickness;
    if (outer < kEpsilon)
        return 0.0f;
    const f32 f = inner / outer;
    const f32 f2 = f * f;
    const f32 rem = 1.0f - f2;
    // NO DRAW when the annulus has zero width. The stream is positional, so
    // consuming one here would desynchronise every later channel.
    const f32 t = (rem != 0.0f) ? (f2 + rem * rng.NextUnit()) : f2;
    return outer * std::sqrt(std::fmax(t, 0.0f));
}

Vector3f SamplePointOnSphere(MwcRng& rng, f32 radius) {
    const f32 u = rng.NextUnit();
    const f32 elev = AsinFast(u + u - 1.0f);
    const f32 phi = rng.NextUnit() * kAzimuthTwoPi;
    const f32 ce = std::cos(elev);
    return {std::sin(phi) * radius * ce, std::cos(phi) * radius * ce, std::sin(elev) * radius};
}

Vector3f SamplePointOnHemisphere(MwcRng& rng, f32 radius) {
    // The ONLY difference from the sphere: the elevation draw is [0,1) rather
    // than [-1,1), so the result never leaves the +Z half.
    const f32 elev = AsinFast(rng.NextUnit());
    const f32 phi = rng.NextUnit() * kAzimuthTwoPi;
    const f32 ce = std::cos(elev);
    return {std::sin(phi) * radius * ce, std::cos(phi) * radius * ce, std::sin(elev) * radius};
}

Vector3f SamplePointOnCircleXY(MwcRng& rng, f32 radius) {
    const f32 phi = rng.NextUnit() * kAzimuthTwoPi;
    return {std::cos(phi) * radius, std::sin(phi) * radius, 0.0f};
}

// ---------------------------------------------------------------------------
// Emission
// ---------------------------------------------------------------------------

Emitter::EmitContext Emitter::BuildEmitContext(const EvalCtx& ec) const {
    const EmitterDesc& d = *d3desc_;

    EmitContext c;
    c.shape = d.shape;

    switch (d.shape) {
    case Shape::SphereShell:
    case Shape::HemisphereShell:
        d.shapeExtent0.ScalarEndpoints(ec, c.ext0Lo, c.ext0Hi);
        break;
    case Shape::Cylinder:
    case Shape::Ring:
        d.shapeExtent0.ScalarEndpoints(ec, c.ext0Lo, c.ext0Hi);
        d.shapeExtent1.ScalarEndpoints(ec, c.ext1Lo, c.ext1Hi);
        break;
    case Shape::Box: {
        // The extent-2 VECTOR path evaluated at r = (0,0,0) and r = (1,1,1),
        // which recovers the low and high corner of the node's random range.
        const Vector4f lo = SampleAt(d.shapeExtent2, {0, 0, 0, 0}, ec);
        const Vector4f hi = SampleAt(d.shapeExtent2, {1, 1, 1, 1}, ec);
        c.boxLo = {lo.x, lo.y, lo.z};
        c.boxHi = {hi.x, hi.y, hi.z};
        break;
    }
    default:
        // Point, and the three mesh shapes. A mesh shape with no bound actor
        // falls through to the point case in the engine too, so this is not a
        // simplification — it is the same branch.
        break;
    }
    return c;
}

// `base` arrives in renderer units (it came off the emitter's world matrix);
// everything this function computes is authored, so it leaves in `.prt` units
// and is converted on the way out. See SetUnitScale.
Vector3f Emitter::SkinEmitMeshVertex(const EmitMesh& m, u32 vertex) const {
    const Vector3f& rest = m.rest[vertex];
    if (m.bones.empty() || surface_.pose.empty() || surface_.invBind.empty())
        return rest;

    const auto& b = m.bones[vertex];
    const auto& lane = m.weights[vertex];
    Vector3f acc{0, 0, 0};
    f32 sum = 0.0f;
    // Four slots since the promotion out of d3::; a `.prt` never fills the
    // fourth, and the weight test below is what makes that free rather than
    // a behaviour change.
    for (usize k = 0; k < kEmitMeshBones; ++k) {
        if (!(lane[k] > 0.0f) || b[k] < 0 || b[k] >= static_cast<i32>(surface_.pose.size()) ||
            b[k] >= static_cast<i32>(surface_.invBind.size()))
            continue;
        const Vector3f p = whiteout::transform_point(
            rest, surface_.invBind[static_cast<usize>(b[k])] * surface_.pose[static_cast<usize>(b[k])]);
        acc = {acc.x + p.x * lane[k], acc.y + p.y * lane[k], acc.z + p.z * lane[k]};
        sum += lane[k];
    }
    if (!(sum > kEpsilon))
        return rest;
    const f32 inv = 1.0f / sum;
    return {acc.x * inv, acc.y * inv, acc.z * inv};
}

bool Emitter::SampleEmitMeshPoint(bool sequential, u32 sequence, Vector3f& out) {
    if (!surface_.mesh || surface_.mesh->Empty())
        return false;
    const EmitMesh& m = *surface_.mesh;

    // Which sub-object. The engine reads the system's own index and draws
    // uniformly when it is -1, with the modulo shortcut it uses everywhere a
    // count might be a power of two; nothing in a viewer ever sets one, so
    // this is always the draw.
    const u32 subCount = static_cast<u32>(m.subs.size());
    const u32 r = sysRng_.Next();
    const EmitMesh::SubMesh& sm =
        m.subs[(((subCount - 1) & subCount) != 0) ? (r % subCount) : (r & (subCount - 1))];
    if (sm.triCount == 0)
        return false;

    u32 tri;
    if (sequential) {
        // Shape 11 walks its triangles in order off a counter that is never
        // reset, so consecutive emissions spread over the surface instead of
        // clustering the way a random draw does.
        tri = sm.firstTri + (sequence % sm.triCount);
    } else {
        // Area-uniform, from the running sum. One draw, because the engine
        // takes one — the stream is positional and an extra draw here shifts
        // every value after it.
        const u32 last = sm.firstTri + sm.triCount - 1;
        const f32 pick = sysRng_.NextUnit() * m.areaCdf[last];
        tri = last;
        for (u32 i = sm.firstTri; i <= last; ++i) {
            if (m.areaCdf[i] >= pick) {
                tri = i;
                break;
            }
        }
    }

    const Vector3f p0 = SkinEmitMeshVertex(m, m.tris[tri * 3 + 0]);
    const Vector3f p1 = SkinEmitMeshVertex(m, m.tris[tri * 3 + 1]);
    const Vector3f p2 = SkinEmitMeshVertex(m, m.tris[tri * 3 + 2]);

    // Two draws folded into the triangle: when they land outside it the SECOND
    // is mirrored and the first's complement is taken, which is the engine's
    // arithmetic rather than the usual `if (a+b>1) { a=1-a; b=1-b; }`.
    const f32 a = sysRng_.NextUnit();
    f32 b = sysRng_.NextUnit();
    f32 w1 = a;
    if (a + b > 1.0f) {
        b = 1.0f - b;
        w1 = 1.0f - a;
    }
    const f32 w0 = (1.0f - w1) - b;
    const Vector3f local{p0.x * w0 + p1.x * w1 + p2.x * b, p0.y * w0 + p1.y * w1 + p2.y * b,
                         p0.z * w0 + p1.z * w1 + p2.z * b};
    out = whiteout::transform_point(local, surface_.toWorld);
    return true;
}

Vector3f Emitter::SampleShape(EmitContext& ec, const Vector3f& base) {
    Vector3f local{0, 0, 0};
    const f32 u = UnitScale();

    switch (ec.shape) {
    case Shape::SphereShell: {
        const f32 r = SampleRadiusInAnnulus(sysRng_, ec.ext0Lo, ec.ext0Hi - ec.ext0Lo);
        local = SamplePointOnSphere(sysRng_, r);
        break;
    }
    case Shape::HemisphereShell: {
        const f32 r = SampleRadiusInAnnulus(sysRng_, ec.ext0Lo, ec.ext0Hi - ec.ext0Lo);
        local = SamplePointOnHemisphere(sysRng_, r);
        break;
    }
    case Shape::Cylinder: {
        const f32 r = SampleRadiusInAnnulus(sysRng_, ec.ext0Lo, ec.ext0Hi - ec.ext0Lo);
        local = SamplePointOnCircleXY(sysRng_, r);
        local.z = ec.ext1Lo;
        if (ec.ext1Hi - ec.ext1Lo != 0.0f)
            local.z += (ec.ext1Hi - ec.ext1Lo) * sysRng_.NextUnit();
        break;
    }
    case Shape::Ring: {
        const f32 r = SampleRadiusInAnnulus(sysRng_, ec.ext0Lo, ec.ext0Hi - ec.ext0Lo);
        // Not random: the azimuth is the emit index spread evenly across the
        // particles being emitted THIS TICK, starting from one random phase
        // drawn at index 0. n spokes, not n scattered points.
        f32 phi;
        if (ec.emitIndex != 0 && ec.emitCount > 0) {
            phi = ec.ringPhase +
                  // The SPACING keeps the real 2pi -- SampleEmitterShape loads the
                  // azimuth constant exactly once, for the index-0 draw above.
                  (static_cast<f32>(ec.emitIndex) * kTwoPi) / static_cast<f32>(ec.emitCount);
            phi = WrapAngle(phi);
        } else {
            phi = sysRng_.NextUnit() * kAzimuthTwoPi;
            ec.ringPhase = phi;
        }
        local = {std::cos(phi) * r, std::sin(phi) * r, 0.0f};
        local.z = ec.ext1Lo;
        if (ec.ext1Hi - ec.ext1Lo != 0.0f)
            local.z += (ec.ext1Hi - ec.ext1Lo) * sysRng_.NextUnit();
        break;
    }
    case Shape::Box: {
        f32 v[3];
        const f32 lo[3] = {ec.boxLo.x, ec.boxLo.y, ec.boxLo.z};
        const f32 hi[3] = {ec.boxHi.x, ec.boxHi.y, ec.boxHi.z};
        for (i32 k = 0; k < 3; ++k) {
            const f32 span = hi[k] - lo[k];
            v[k] = lo[k];
            if (span != 0.0f)
                v[k] += span * sysRng_.NextUnit();
        }
        // The box case adds the base but NOT the emit-context offset. It is
        // the only shape that skips it; reproduced, not tidied.
        return {base.x + v[0] * u, base.y + v[1] * u, base.z + v[2] * u};
    }
    case Shape::MeshRandom:
    case Shape::MeshActorKind4:
    case Shape::MeshSequential: {
        // The counter advances whichever of the three this is — only shape 11
        // reads it, and the engine increments it in TickEmitter before the
        // dispatch rather than inside the sampler.
        const u32 seq = emitSequence_++;
        Vector3f p;
        if (SampleEmitMeshPoint(ec.shape == Shape::MeshSequential, seq, p))
            return p; // a surface point REPLACES the base; it is not an offset
        break;        // no surface bound: the engine's own point-case fallback
    }
    case Shape::Point:
    default:
        break;
    }

    return {base.x + (ec.offset.x + local.x) * u, base.y + (ec.offset.y + local.y) * u,
            base.z + (ec.offset.z + local.z) * u};
}

} // namespace whiteout::flakes::renderer::particle::d3
