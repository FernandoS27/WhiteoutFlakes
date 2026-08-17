//===----------------------------------------------------------------------===//
// snowball/cloth.cpp -- the cloth solver: one constraint pass per step, no iteration count.
//
// The formulas here are exact down to operand association: solver divisions and
// normalisations go through Rcp/Rsqrt estimates plus Newton refinement, and where a pass
// carries its own association (the bend module, the segment cast) the difference from
// math.h's forms is one ulp and still deliberate -- results are compared bit-for-bit, so
// "equivalent" arithmetic is not equivalent.
//
// Two behaviours below look like bugs and are engine contract, kept on purpose: the
// PreSolve hard/soft LATCH (once one transform group teleports hard, every later soft group
// inherits the hard side-effects) and the Create/SetScale bend-lane disagreement (each
// scales a different lane of the bend records). Fixing either changes how cloth settles.
//===----------------------------------------------------------------------===//
#include "snowball/cloth.h"

#include <bit>
#include <cmath>

#include "snowball/math.h"

namespace snowball {
namespace {

// The cloth family's own degenerate guard: 0x057A0000, distinct from kZeroSafe.
const f32 kClothTiny = std::bit_cast<f32>(u32{0x057A0000});

// Bend-sweep constants: the 0.28 atan Pade coefficient, pi (pi/2 is derived from it at the
// point of use), and the two degenerate guards.
const f32 kAtanPade = std::bit_cast<f32>(u32{0x3E8F5C29});
const f32 kPi = std::bit_cast<f32>(u32{0x40490FDB});
const f32 kBendNormalGuard = std::bit_cast<f32>(u32{0x302BCC76});  // ~6.25e-10, on min |n|^2
const f32 kBendEdgeGuard = std::bit_cast<f32>(u32{0x37D1B717});    // 2.5e-5, on |e|^2

// The radial impulse's per-particle cap -- same value as the wind clamp, distinct global.
constexpr f32 kRadialClamp = 0.25f;

// The teleport threshold: a transform group whose pinned particles moved more than this
// (squared) in one step is advected rigidly instead of pulled.
constexpr f32 kTeleportThresholdSq = 0.5f;

// Wind-impulse constants: the per-component velocity clamp and the per-vertex area share
// (0.333f exactly -- a typed decimal, NOT float(1/3): the aero velocity gain's third IS
// 0.33333334f, and the two must not be unified).
constexpr f32 kWindClamp = 0.25f;
constexpr f32 kAreaShare = 0.333f;

// v + 2*cross(q, w*v + cross(q, v)) -- the quat-rotate expansion the cloth code inlines.
Vec4 Rotate(const Vec4& q, const Vec4& v) {
    const Vec4 t = v * q.w + Cross3(q, v);
    return v + Cross3(q, t) * 2.0f;
}

Vec4 InverseRotate(const Vec4& q, const Vec4& v) {
    const Vec4 t = v * q.w + Cross3(v, q);
    return v + Cross3(t, q) * 2.0f;
}

// rcp with the zero guard the velocity recompute uses: 0 when |v| is degenerate.
f32 GuardedRcp(f32 v) {
    return (kClothTiny < std::max(-v, v)) ? Rcp(v) : 0.0f;
}

// The bend module refines its estimates with its OWN operand associations, one ulp apart
// from math.h's forms (which are the capsule module's) -- the split is deliberate, not a
// cleanup target. RsqrtBend is form A, RcpBend form C (unguarded), NegRcpBend form E
// (= -rcpNR).
#if SNOWBALL_HAS_SSE_RECIPROCAL
f32 RsqrtBend(f32 v) {
    const __m128 x = _mm_set1_ps(v);
    const __m128 y = _mm_rsqrt_ps(x);
    const __m128 half = _mm_set1_ps(0.5f);
    const __m128 t =
        _mm_sub_ps(half, _mm_mul_ps(_mm_mul_ps(_mm_mul_ps(y, y), half), x));
    return _mm_cvtss_f32(_mm_add_ps(_mm_mul_ps(t, y), y));
}
f32 RcpBend(f32 v) {
    const __m128 x = _mm_set1_ps(v);
    const __m128 y = _mm_rcp_ps(x);
    const __m128 t = _mm_sub_ps(_mm_set1_ps(1.0f), _mm_mul_ps(x, y));
    return _mm_cvtss_f32(_mm_add_ps(_mm_mul_ps(t, y), y));
}
f32 NegRcpBend(f32 v) {
    const __m128 x = _mm_set1_ps(v);
    const __m128 y = _mm_rcp_ps(x);
    const __m128 t = _mm_sub_ps(_mm_mul_ps(x, y), _mm_set1_ps(1.0f));
    return _mm_cvtss_f32(_mm_sub_ps(_mm_mul_ps(t, y), y));
}
#else
f32 RsqrtBend(f32 v) { return 1.0f / std::sqrt(v); }
f32 RcpBend(f32 v) { return 1.0f / v; }
f32 NegRcpBend(f32 v) { return -1.0f / v; }
#endif

// Componentwise clamp to [-limit, +limit], min first and then max.
Vec4 ClampComponents(const Vec4& v, f32 limit) {
    return {std::max(std::min(v.x, limit), -limit), std::max(std::min(v.y, limit), -limit),
            std::max(std::min(v.z, limit), -limit), std::max(std::min(v.w, limit), -limit)};
}

f32 Clamp01(f32 v) { return std::max(0.0f, std::min(v, 1.0f)); }

}  // namespace

bool ClassifyClothCapsule(const ClothCapsuleDesc& desc, ClothCapsuleDef& out) {
    // The record is fully written on EVERY path, the degenerate one included -- its
    // featureType stays 0, which the narrowphase dispatches into the tapered branch rather
    // than rejecting; see the header note on the return value.
    out.localRotation = desc.localRotation;
    out.localPosition = desc.localPosition;
    const auto clampAxis = [](f32 v) { return std::max(std::min(v, 2.0f), 0.5f); };
    out.radii = {clampAxis(desc.scale.x), clampAxis(desc.scale.y), clampAxis(desc.scale.z),
                 1.0f};
    out.inverseRadii = {Rcp(out.radii.x), Rcp(out.radii.y), Rcp(out.radii.z), Rcp(1.0f)};
    out.anchor = desc.anchor;
    out.featureType = 0;

    f32 r0 = desc.radius0;
    f32 r1 = desc.radius1;
    const f32 length = desc.length;
    if (r0 <= 0.005f || r1 <= 0.005f || length < 0.0f) {
        out.radius0 = r0;
        out.radius1 = r1;
        out.fullLength = length;
        out.taper = {length, 0.0f, 1.0f, 1.0f};
        return false;
    }
    const f32 delta = r0 - r1;
    if (length < kClothTiny || length < std::abs(delta)) {
        out.featureType = 1;  // sphere: both ends collapse to the larger radius
    } else if (std::abs(delta) >= 0.005f) {
        out.featureType = 3;  // tapered cone between two unequal end spheres
        const f32 chordSq = length * length - delta * delta;
        f32 chord = 0.0f;
        if (chordSq != 0.0f) {
            // sqrt via an exact-seed refinement polynomial rather than std::sqrt alone:
            // (0.5*x*r)*(3 - x*r^2) with r = 1/sqrt(x) from a true divide.
            const f32 r = 1.0f / std::sqrt(chordSq);
            chord = (0.5f * chordSq * r) * (3.0f - chordSq * (r * r));
        }
        const f32 invLength = 1.0f / length;
        out.radius0 = r0;
        out.radius1 = r1;
        out.fullLength = length;
        out.taper = {chord, delta / length, chord * invLength, desc.pushScale};
        return true;
    } else {
        out.featureType = 2;  // near-equal radii: a plain segment
    }
    const f32 maxR = std::max(r0, r1);
    out.radius0 = maxR;
    out.radius1 = maxR;
    out.fullLength = length;
    out.taper = {length, 0.0f, 1.0f, desc.pushScale};
    return true;
}

void Cloth::Create(const ClothDef& def) {
    params_ = def.params;
    worldScale_ = std::max(def.worldScale, 0.01f);
    worldPrev_ = worldCur_ = def.world;
    windNoise_ = {};
    pinnedCount_ = static_cast<i32>(def.pinnedCount);
    forceTeleport_ = false;
    groups_.assign(def.transformGroupCount, {});
    frameBind_ = nullptr;
    frameAnim_ = nullptr;
    worldCapsules_.clear();
    worldPlanes_.clear();

    // Anchor state: cur = prev = the def's bind pose, mapped slot -> compact index. The
    // deltas start at identity; the first Step overwrites every mapped slot before use.
    anchorMap_ = def.anchorMap;
    anchors_.assign(def.anchorStateCount, {});
    anchorDeltas_.assign(def.anchorStateCount, {});
    for (usize slot = 0; slot < anchorMap_.size(); ++slot) {
        const i16 c = anchorMap_[slot];
        if (c >= 0 && slot < def.bindPose.size()) {
            anchors_[c] = def.bindPose[slot];
        }
    }

    const f32 s = worldScale_;
    hasSelfCollision_ = false;
    selfCollisionEnabled_ = false;
    particles_.assign(def.particles.size(), {});
    for (usize i = 0; i < def.particles.size(); ++i) {
        const ClothParticleDef& in = def.particles[i];
        Particle& p = particles_[i];
        p.restPosition = in.restPosition;
        p.restNormal = in.restNormal;
        p.skinnedRestNormal = in.skinnedRestNormal;
        p.refRows[0] = in.refRow0;
        p.refRows[1] = in.refRow1;
        p.refRows[2] = in.refRow2;
        p.refRows[3] = in.refRow3;
        for (i32 k = 0; k < 4; ++k) {
            p.anchorSlots[k] = in.anchorSlots[k];
        }
        p.anchorWeights = in.anchorWeights;
        p.authoredInverseMass = in.inverseMass;
        p.tetherRestLength = in.tetherRestLength;
        p.tetherRoot = in.tetherRoot;
        p.lutIndex = in.lutIndex;
        p.group = in.transformGroup;
        p.frameReference = in.frameReference;
        p.proxy = in.collisionProxy < 0 ? static_cast<i32>(i) : in.collisionProxy;
        p.selfFlag = in.selfCollision;
        hasSelfCollision_ |= p.selfFlag;
        p.position = Rotate(worldCur_.rotation, in.restPosition * s) + worldCur_.position;
        p.prevPosition = p.position;
        p.integrationStart = p.position;
    }

    // The inverse-mass rescale and the derived constraint weights.
    // Only dynamic particles get a scaled mass; a pinned endpoint contributes 0 and an edge
    // with two pinned ends guards its weights to zero instead of dividing.
    const f32 invMassScale = (1.0f / params_.massMultiplier) * (1.0f / (s * s));
    for (i32 i = pinnedCount_; i < ParticleCount(); ++i) {
        particles_[i].scaledInverseMass = particles_[i].authoredInverseMass * invMassScale;
    }

    tetherLut_.assign(def.tetherLutCount + 1, 0.0f);
    const f32 lutStep = def.tetherLutCount ? 1.0f / static_cast<f32>(def.tetherLutCount) : 0.0f;
    invTetherLutCount_ = lutStep;
    for (usize i = 0; i < tetherLut_.size(); ++i) {
        tetherLut_[i] = std::pow(static_cast<f32>(i) * lutStep, params_.tetherExponent) *
                        params_.tetherScale;
    }

    edges_.assign(def.edges.size(), {});
    maxScaledRestEdge_ = 0.0f;
    for (usize k = 0; k < def.edges.size(); ++k) {
        const ClothEdgeDef& in = def.edges[k];
        Edge& e = edges_[k];
        e.a = in.a;
        e.b = in.b;
        e.restLength = in.restLength * s;
        e.blend1 = in.stiffnessBlend1;
        e.blend2 = in.stiffnessBlend2;
        e.selfMask = in.selfCollisionMask;
        const f32 sum = particles_[e.a].scaledInverseMass + particles_[e.b].scaledInverseMass;
        const f32 w = sum > kClothTiny ? 1.0f / sum : 0.0f;
        e.weightA = particles_[e.a].scaledInverseMass * w;
        e.weightB = particles_[e.b].scaledInverseMass * w;
        maxScaledRestEdge_ = std::max(maxScaledRestEdge_, e.restLength);
    }

    // Bend records: the four endpoints' scaled inverse masses, plus pair weights derived
    // from the FIRST two endpoints only (same both-pinned guard as the distance records);
    // Create scales the correction clamps by worldScale squared.
    bends_.assign(def.bends.size(), {});
    for (usize k = 0; k < def.bends.size(); ++k) {
        const ClothBendDef& in = def.bends[k];
        Bend& b = bends_[k];
        b.e0 = in.e0;
        b.e1 = in.e1;
        b.e2 = in.e2;
        b.e3 = in.e3;
        b.restAngle = in.restAngle;
        b.selfMask = in.selfCollisionMask;
        b.clampA = in.clampA * (s * s);
        b.clampB = in.clampB * (s * s);
        const u16 ends[4] = {b.e0, b.e1, b.e2, b.e3};
        for (i32 j = 0; j < 4; ++j) {
            b.invMass[j] = particles_[ends[j]].scaledInverseMass;
        }
        const f32 sum = b.invMass[0] + b.invMass[1];
        const f32 w = sum > kClothTiny ? 1.0f / sum : 0.0f;
        b.weightA = b.invMass[0] * w;
        b.weightB = b.invMass[1] * w;
    }

    triangles_.assign(def.triangles.size(), {});
    for (usize k = 0; k < def.triangles.size(); ++k) {
        const ClothTriangleDef& in = def.triangles[k];
        Triangle& t = triangles_[k];
        t.a = in.a;
        t.b = in.b;
        t.c = in.c;
        t.area = in.area * (s * s);
        t.selfCollision = in.selfCollision;
    }

    capsules_.assign(def.capsules.size(), {});
    for (usize k = 0; k < def.capsules.size(); ++k) {
        const ClothCapsuleDef& in = def.capsules[k];
        Capsule& c = capsules_[k];
        c.localRotation = in.localRotation;
        c.localPosition = in.localPosition;
        c.radii = in.radii;
        c.inverseRadii = in.inverseRadii;
        c.taper = in.taper;
        c.radius0 = in.radius0;
        c.radius1 = in.radius1;
        c.fullLength = in.fullLength;
        c.featureType = in.featureType;
        c.anchor = in.anchor;
    }

    planes_.assign(def.planes.size(), {});
    for (usize k = 0; k < def.planes.size(); ++k) {
        const ClothPlaneDef& in = def.planes[k];
        Plane& pl = planes_[k];
        pl.localRotation = in.localRotation;
        pl.localPosition = in.localPosition;
        pl.pushScale = in.pushScale;
        pl.anchor = in.anchor;
    }

    // Create runs the step advection once with the BIND pose as the anchor array, then
    // copies cur -> prev, so the first step sees no collider motion.
    AdvanceColliders(def.bindPose.data());
    for (Capsule& c : capsules_) {
        c.prevRotation = c.rotation;
        c.prevPosition = c.position;
    }
    for (Plane& pl : planes_) {
        pl.prevRotation = pl.rotation;
        pl.prevPosition = pl.position;
    }

    BuildOutputFrames();
}

void Cloth::Step(const ClothFrame& frame) {
    const f32 dt = frame.dt;
    if (dt < kClothTiny) {
        return;
    }
    worldPrev_ = worldCur_;
    worldCur_ = frame.world;
    windNoise_ = frame.windNoise;
    frameBind_ = frame.bind;
    frameAnim_ = frame.anim;

    AdvanceAnchors(frame);
    AdvanceColliders(frame.anim);
    StageWorldColliders(frame);

    for (i32 i = pinnedCount_; i < ParticleCount(); ++i) {
        particles_[i].prevPosition = particles_[i].position;
    }

    const bool teleported = PreSolve();
    if (!teleported) {
        WindImpulse(dt);
    }
    Integrate(dt);

    // One constraint pass per step -- there is no iteration count. Self-collision mode skips
    // the tether pass entirely; the collider pass runs capsules first, then half-spaces.
    SolveDistance();
    SolveBend();
    SolveDistance();
    if ((params_.attachmentsEnabled || params_.tetherStiffness > 0.0f) &&
        !selfCollisionEnabled_) {
        SolveTethers();
        SolveAttachments();
    }
    if (params_.hasColliders) {
        CollideCapsules();
        CollidePlanes();
    }
    RestPosePull();
    RecomputeVelocities(dt);
    BuildOutputFrames();
    frameBind_ = nullptr;
    frameAnim_ = nullptr;
}

void Cloth::AdvanceAnchors(const ClothFrame& frame) {
    // Step stage 3: per mapped model slot, roll the anchor state and derive the per-step
    // affine delta -- a point at x last frame rides to Rotate(dq, x - old.pos) + new.pos.
    if (frame.anim == nullptr) {
        return;
    }
    for (usize slot = 0; slot < anchorMap_.size(); ++slot) {
        const i16 c = anchorMap_[slot];
        if (c < 0) {
            continue;
        }
        const ClothAnchor old = anchors_[c];
        const ClothAnchor& fresh = frame.anim[slot];
        anchors_[c] = fresh;
        const Vec4 conj{-old.rotation.x, -old.rotation.y, -old.rotation.z, old.rotation.w};
        const Vec4 dq = QuatMultiply(fresh.rotation, conj);
        anchorDeltas_[c].rotation = dq;
        // = fresh.pos - Rotate(dq, old.pos), associated as the position difference
        // first, then the -2 cross term.
        const Vec4 t = old.position * dq.w + Cross3(dq, old.position);
        anchorDeltas_[c].position =
            (fresh.position - old.position) + Cross3(dq, t) * -2.0f;
    }
}

void Cloth::AdvanceColliders(const ClothAnchor* anim) {
    // Step stage 4: the local pose rides its anchor bone (or the bare cloth transform when
    // anchored to -1), then the cloth's world transform; the previous pose is kept so the
    // friction pass can see the collider surface's own motion. The local position is stored
    // UNSCALED -- worldScale enters here, at use.
    const f32 s = worldScale_;
    for (Capsule& c : capsules_) {
        c.prevRotation = c.rotation;
        c.prevPosition = c.position;
        Vec4 q = c.localRotation;
        Vec4 pModel = c.localPosition * s;
        if (c.anchor >= 0 && anim != nullptr) {
            const ClothAnchor& a = anim[c.anchor];
            q = QuatMultiply(a.rotation, c.localRotation);
            pModel = (Rotate(a.rotation, c.localPosition) + a.position) * s;
        }
        c.rotation = QuatMultiply(worldCur_.rotation, q);
        c.position = Rotate(worldCur_.rotation, pModel) + worldCur_.position;
    }
    for (Plane& pl : planes_) {
        pl.prevRotation = pl.rotation;
        pl.prevPosition = pl.position;
        Vec4 q = pl.localRotation;
        Vec4 pModel = pl.localPosition * s;
        if (pl.anchor >= 0 && anim != nullptr) {
            const ClothAnchor& a = anim[pl.anchor];
            q = QuatMultiply(a.rotation, pl.localRotation);
            pModel = (Rotate(a.rotation, pl.localPosition) + a.position) * s;
        }
        pl.rotation = QuatMultiply(worldCur_.rotation, q);
        pl.position = Rotate(worldCur_.rotation, pModel) + worldCur_.position;
        pl.axis = Rotate(pl.rotation, constants::kUnitZ);
    }
}

void Cloth::StageWorldColliders(const ClothFrame& frame) {
    // The world-collider staging: fully rewritten from the frame each step. A degenerate
    // world capsule KEEPS its slot as a classified type-0 record with identity poses -- the
    // count is deliberately not adjusted, so the slot still collides.
    worldCapsules_.assign(static_cast<usize>(std::max(frame.worldCapsuleCount, 0)), {});
    for (i32 k = 0; k < frame.worldCapsuleCount; ++k) {
        const ClothWorldCapsule& src = frame.worldCapsules[k];
        ClothCapsuleDesc desc;
        desc.radius0 = src.radius0;
        desc.radius1 = src.radius1;
        desc.length = src.fullLength;
        desc.pushScale = 1.0f;
        desc.anchor = 0;
        ClothCapsuleDef rec;
        Capsule& dst = worldCapsules_[static_cast<usize>(k)];
        if (ClassifyClothCapsule(desc, rec)) {
            dst.prevRotation = src.prevRotation;
            dst.prevPosition = src.prevPosition;
            dst.rotation = src.rotation;
            dst.position = src.position;
        } else {
            // Degenerate: the poses stay the classifier's identity seeds -- and the
            // record still collides, through the tapered branch.
            dst.prevRotation = dst.rotation = constants::kQuatIdentity;
        }
        dst.localRotation = rec.localRotation;
        dst.localPosition = rec.localPosition;
        dst.radii = rec.radii;
        dst.inverseRadii = rec.inverseRadii;
        dst.taper = rec.taper;
        dst.radius0 = rec.radius0;
        dst.radius1 = rec.radius1;
        dst.fullLength = rec.fullLength;
        dst.featureType = rec.featureType;
        dst.anchor = rec.anchor;
    }

    worldPlanes_.assign(static_cast<usize>(std::max(frame.worldSphereCount, 0)), {});
    for (i32 k = 0; k < frame.worldSphereCount; ++k) {
        const ClothWorldSphere& src = frame.worldSpheres[k];
        Plane& dst = worldPlanes_[static_cast<usize>(k)];
        dst.prevRotation = src.prevRotation;
        dst.prevPosition = src.prevPosition;
        dst.rotation = src.rotation;
        dst.position = src.position;
        dst.localRotation = src.rotation;   // current pose doubles as the local pose here
        dst.localPosition = src.position;
        dst.axis = Rotate(src.rotation, constants::kUnitZ);
        dst.pushScale = src.pushScale;      // often only ever the default -- see the .h note
        dst.anchor = -1;
    }
}

void Cloth::EnableSelfCollision() {
    // The one-way self-collision switch. Flagged particles lose BOTH masses and their
    // velocities; the constraint masks -- not the zeroed masses -- are what disable the
    // records touching them (the record weights were baked at Create).
    if (!hasSelfCollision_) {
        return;
    }
    for (i32 i = pinnedCount_; i < ParticleCount(); ++i) {
        Particle& p = particles_[i];
        if (!p.selfFlag) {
            continue;
        }
        p.authoredInverseMass = 0.0f;
        p.scaledInverseMass = 0.0f;
        p.velocity = {};
        p.auxVelocity = {};
    }
    selfCollisionEnabled_ = true;
}

f32 Cloth::ApplyRadialImpulse(const Vec4& center, f32 radius, f32 strength) {
    if (params_.radialImpulseScale == 0.0f) {
        return 0.0f;
    }
    const f32 k = params_.radialImpulseScale * strength;
    f32 ret = 0.0f;
    for (i32 i = pinnedCount_; i < ParticleCount(); ++i) {
        Particle& p = particles_[i];
        if (selfCollisionEnabled_ && p.selfFlag) {
            continue;
        }
        const Vec4 d = p.position - center;
        const f32 dSq = Dot3(d, d);
        if (dSq <= radius * radius && kClothTiny < dSq) {
            const Vec4 dir = d * Rsqrt(dSq);
            f32 magnitude = p.scaledInverseMass * k;
            if (p.selfFlag) {
                ret = std::max(ret, magnitude);  // reported PRE-clamp
            }
            magnitude = std::min(magnitude, kRadialClamp);
            p.velocity = p.velocity + dir * magnitude;
        }
    }
    return ret;
}

bool Cloth::PreSolve() {
    for (GroupScratch& g : groups_) {
        g = {};
    }
    const bool anchored = !anchorMap_.empty() && frameBind_ != nullptr;

    // Pinned particles go exactly where the transforms put their rest position -- the
    // 4-way anchor skin when the cloth is anchored, the bare world transform otherwise --
    // and each transform group records the largest single-step displacement its pins took.
    const f32 s = worldScale_;
    for (i32 i = 0; i < pinnedCount_; ++i) {
        Particle& p = particles_[i];
        Vec4 target;
        if (anchored) {
            // bind by RAW slot; anim through the compaction map into this frame's anchor
            // state. The scale converts unscaled model-local to world AFTER the blend.
            const auto bone = [&](i32 k) {
                const ClothAnchor& b = frameBind_[p.anchorSlots[k]];
                const ClothAnchor& a = anchors_[anchorMap_[p.anchorSlots[k]]];
                return Rotate(a.rotation,
                              InverseRotate(b.rotation, p.restPosition - b.position)) +
                       a.position;
            };
            Vec4 model = bone(0) * p.anchorWeights.x;
            model = model + bone(1) * p.anchorWeights.y;
            model = model + bone(2) * p.anchorWeights.z;
            model = model + bone(3) * p.anchorWeights.w;
            target = Rotate(worldCur_.rotation, model * s) + worldCur_.position;
        } else {
            target = Rotate(worldCur_.rotation, p.restPosition * s) + worldCur_.position;
        }
        if (p.group >= 0) {
            GroupScratch& g = groups_[p.group];
            const Vec4 d = target - p.position;
            g.active = true;
            g.maxDisplacementSq = std::max(g.maxDisplacementSq, Dot3(d, d));
        }
        p.position = target;
    }

    // The hard/soft sweep, ascending group order -- with the LATCH the file comment names:
    // once any group goes hard, every LATER soft group also gets the hard side-effects
    // (velocities zeroed, prevPosition rewritten) while still blending with its own soft
    // factor. `anyHard` is deliberately never reset; resetting it per group changes how
    // mixed-group teleports settle.
    bool anyHard = false;
    for (usize slot = 0; slot < groups_.size(); ++slot) {
        if (!groups_[slot].active) {
            continue;
        }
        const bool hard =
            forceTeleport_ || groups_[slot].maxDisplacementSq > kTeleportThresholdSq;
        anyHard |= hard;
        const f32 factor = hard ? 1.0f : params_.worldFollowStiffness;
        for (i32 i = pinnedCount_; i < ParticleCount(); ++i) {
            Particle& p = particles_[i];
            if (p.group != static_cast<i16>(slot)) {
                continue;
            }
            Vec4 corrected;
            if (anchored) {
                // Ride the borrowed root's anchors: strip the previous world transform and
                // the scale, apply the per-step anchor deltas, blend by the root's weights,
                // then scale and re-transform. The blend leans from full follow at the
                // pinned boundary toward the soft factor with hop distance (the lut lerp
                // exists ONLY on this path).
                const Particle& root = particles_[p.tetherRoot];
                const Vec4 local =
                    InverseRotate(worldPrev_.rotation, p.position - worldPrev_.position) *
                    GuardedRcp(s);
                const auto ride = [&](i32 k) {
                    // = Rotate(delta.q, local) + delta.pos, associated as (pos + local)
                    // first, then the doubled cross term.
                    const i16 c = anchorMap_[root.anchorSlots[k]];
                    const ClothAnchor& d = anchorDeltas_[c < 0 ? 0 : c];
                    const Vec4 t = local * d.rotation.w + Cross3(d.rotation, local);
                    return (d.position + local) + Cross3(d.rotation, t) * 2.0f;
                };
                Vec4 adv = ride(0) * root.anchorWeights.x;
                adv = adv + ride(1) * root.anchorWeights.y;
                adv = adv + ride(2) * root.anchorWeights.z;
                adv = adv + ride(3) * root.anchorWeights.w;
                const Vec4 target =
                    Rotate(worldCur_.rotation, adv * s) + worldCur_.position;
                const f32 f = static_cast<f32>(p.lutIndex) * invTetherLutCount_;
                const f32 blend = f * factor + (1.0f - f);
                corrected = p.position + (target - p.position) * blend;
            } else {
                // Rigid re-transform by the world delta, no lut lerp, no scale round trip.
                const Vec4 target =
                    Rotate(worldCur_.rotation, InverseRotate(worldPrev_.rotation,
                                                             p.position - worldPrev_.position)) +
                    worldCur_.position;
                corrected = p.position + (target - p.position) * factor;
            }
            if (anyHard) {
                p.velocity = {};
                p.auxVelocity = {};
                p.prevPosition = corrected;
            }
            p.position = corrected;
        }
    }
    forceTeleport_ = false;
    return anyHard;
}

void Cloth::WindImpulse(f32 dt) {
    if (!(params_.aeroNoiseGain > 0.0f || params_.aeroVelocityGain > 0.0f)) {
        return;
    }
    const f32 velocityGain = params_.aeroVelocityGain * 0.33333334f;
    const Vec4 constantFlow =
        -params_.windVelocity - windNoise_ * params_.aeroNoiseGain;
    const f32 impulseScale = params_.windImpulseScale * 0.5f * dt;

    for (const Triangle& t : triangles_) {
        // Self-collision mode drops the flagged triangles from the wind pass.
        if (selfCollisionEnabled_ && t.selfCollision) {
            continue;
        }
        // Flow estimate: two shares of vertex b's velocity, one of vertex a's, minus wind.
        const Vec4 flow =
            (particles_[t.b].auxVelocity * 2.0f + particles_[t.a].auxVelocity) * velocityGain +
            constantFlow;
        const f32 flowSq = Dot3(flow, flow);
        if (!(flowSq > kClothTiny)) {
            continue;
        }
        const Vec4 dir = flow * Rsqrt(flowSq);
        // The cached face normal, sign-matched to the flow: cloth is double-sided.
        const Vec4 n = Dot3(dir, t.normal) > 0.0f ? t.normal : -t.normal;
        const f32 cosine = Dot3(n, dir);
        const Vec4 lift = Cross3(Cross3(n, dir), dir);
        const f32 magnitude = ((flowSq * impulseScale) * (t.area * kAreaShare)) * cosine;
        const Vec4 impulse = (lift * params_.liftFactor - dir) * magnitude;
        for (const u16 v : {t.a, t.b, t.c}) {
            Particle& p = particles_[v];
            Vec4 dv = impulse * p.scaledInverseMass;
            dv = {std::max(std::min(dv.x, kWindClamp), -kWindClamp),
                  std::max(std::min(dv.y, kWindClamp), -kWindClamp),
                  std::max(std::min(dv.z, kWindClamp), -kWindClamp),
                  std::max(std::min(dv.w, kWindClamp), -kWindClamp)};
            p.velocity = p.velocity + dv;
        }
    }
}

void Cloth::Integrate(f32 dt) {
    const f32 damping = std::exp(-(dt * params_.damping));
    const Vec4 gravityDt = params_.gravity * dt;
    for (i32 i = pinnedCount_; i < ParticleCount(); ++i) {
        Particle& p = particles_[i];
        p.integrationStart = p.position;
        p.penetrationDepth = 0.0f;
        // A frozen self-collision proxy still seeds its integration state above; only the
        // velocity and position update are skipped.
        if (selfCollisionEnabled_ && p.selfFlag) {
            continue;
        }
        p.velocity = p.velocity * damping + gravityDt;
        p.position = p.position + p.velocity * dt;
    }
}

void Cloth::SolveDistance() {
    const f32 s0 = std::max(0.0f, std::min(params_.distanceStiffness[0], 1.0f));
    const f32 s1 = std::max(0.0f, std::min(params_.distanceStiffness[1], 1.0f));
    const f32 s2 = std::max(0.0f, std::min(params_.distanceStiffness[2], 1.0f));
    const f32 selfCF = selfCollisionEnabled_ ? 1.0f : 0.0f;

    for (const Edge& e : edges_) {
        Particle& pa = particles_[e.a];
        Particle& pb = particles_[e.b];
        const Vec4 edge = pb.position - pa.position;
        const f32 L2 = e.restLength * e.restLength;
        // The sqrt-free projection: 1 - 2*L0^2 / (L0^2 + |e|^2). Zero correction at rest
        // length, asymptotically full on heavy stretch, negative (push apart) on compression.
        const f32 denom = (L2 + edge.z * edge.z) + (edge.y * edge.y + edge.x * edge.x);
        const f32 factor = -Rcp(denom) * (L2 + L2) + 1.0f;
        const f32 mix = ((1.0f - e.blend1) - e.blend2) * s0 + (e.blend2 * s2 + e.blend1 * s1);
        // The lane-3 mask disables constraints that touch frozen self-collision proxies.
        const Vec4 correction = edge * ((mix * (1.0f - e.selfMask * selfCF)) * factor);
        pa.position = pa.position + correction * e.weightA;
        pb.position = pb.position - correction * e.weightB;
    }
}

void Cloth::SolveBend() {
    // One Gauss-Seidel sweep over the dihedral constraints. The hinge edge is (e2, e3) of
    // the record; e0/e1 are the wings. Angle is a four-quadrant atan2 built from the 0.28
    // Pade; corrections are clamped per COMPONENT, clampA on wing e0, clampB on wing e1,
    // min of both on the hinge ends. Degenerate records still run the clamp-and-store
    // epilogue on a zero correction rather than skipping it.
    const f32 kBend = Clamp01(params_.bendStiffness);
    const f32 selfCF = selfCollisionEnabled_ ? 1.0f : 0.0f;
    const f32 restEnable = params_.killBendRestAngle ? 0.0f : 1.0f;
    const f32 halfPi = kPi * 0.5f;

    for (const Bend& b : bends_) {
        const Vec4 pA = particles_[b.e2].position;  // hinge end A
        const Vec4 pD = particles_[b.e3].position;  // hinge end D
        const Vec4 pB = particles_[b.e0].position;  // wing B
        const Vec4 pC = particles_[b.e1].position;  // wing C

        const Vec4 e1 = pB - pA;
        const Vec4 e2 = pC - pA;
        const Vec4 e = pD - pA;
        const f32 eSq = Dot3(e, e);
        const Vec4 eHat = e * RsqrtBend(eSq);
        const f32 eLen = std::sqrt(eSq);            // one of the three full-sqrt sites

        const Vec4 nA = Cross3(e, e1);
        const Vec4 nB = Cross3(e2, e);
        const f32 nASq = Dot3(nA, nA);
        const f32 nBSq = Dot3(nB, nB);
        const f32 rsA = RsqrtBend(nASq);
        const f32 rsB = RsqrtBend(nBSq);
        const Vec4 nAHat = nA * rsA;
        const Vec4 nBHat = nB * rsB;
        const Vec4 nA1 = nAHat * rsA;               // nA / |nA|^2
        const Vec4 nB1 = nBHat * rsB;

        // Bridson-style gradients; the add order is fixed, not free to reassociate.
        const Vec4 gB = nA1 * eLen;
        const Vec4 gC = nB1 * eLen;
        const Vec4 gA = nB1 * Dot3(pD - pC, eHat) + nA1 * Dot3(pD - pB, eHat);
        const Vec4 gD = nB1 * Dot3(pC - pA, eHat) + nA1 * Dot3(pB - pA, eHat);
        const f32 w = b.invMass[3] * Dot3(gD, gD) +
                      (b.invMass[2] * Dot3(gA, gA) +
                       (b.invMass[1] * Dot3(gC, gC) + b.invMass[0] * Dot3(gB, gB)));
        const f32 invW = GuardedRcp(w);

        // Four-quadrant atan2(sin, cos): the tangent's reciprocal is UNREFINED-guard-free,
        // the two Pade branches split at |t| = 1, cos < 0 wraps by +-pi, cos == 0 is exact.
        const f32 cosT = Dot3(nAHat, nBHat);
        const f32 sinT = Dot3(Cross3(nAHat, nBHat), eHat);
        const f32 t = sinT * RcpBend(cosT);
        const f32 small = t * RcpBend((t * t) * kAtanPade + 1.0f);
        const f32 large = NegRcpBend(t * t + kAtanPade) * t + halfPi;
        f32 theta;
        if (cosT == 0.0f) {
            theta = sinT < 0.0f ? -halfPi : (0.0f < sinT ? halfPi : 0.0f);
        } else if (std::abs(t) < 1.0f) {
            theta = (cosT < 0.0f) ? ((sinT < 0.0f ? -kPi : kPi) + small) : small;
        } else {
            theta = (sinT < 0.0f) ? (large - kPi) : large;
        }

        const bool degenerate =
            (std::min(nASq, nBSq) < kBendNormalGuard) || (eSq < kBendEdgeGuard);
        const f32 lambda = (((1.0f - b.selfMask * selfCF) * kBend) * invW) *
                           (theta - b.restAngle * restEnable);
        const Vec4 dB = degenerate ? Vec4{} : (gB * b.invMass[0]) * lambda;
        const Vec4 dC = degenerate ? Vec4{} : (gC * b.invMass[1]) * lambda;
        const Vec4 dA = degenerate ? Vec4{} : (gA * b.invMass[2]) * (-lambda);
        const Vec4 dD = degenerate ? Vec4{} : (gD * b.invMass[3]) * (-lambda);

        const f32 m = std::min(b.clampA, b.clampB);
        particles_[b.e0].position = pB + ClampComponents(dB, b.clampA);
        particles_[b.e1].position = pC + ClampComponents(dC, b.clampB);
        particles_[b.e2].position = pA + ClampComponents(dA, m);
        particles_[b.e3].position = pD + ClampComponents(dD, m);
    }
}

void Cloth::SolveTethers() {
    if (!(params_.tetherStiffness > 0.0f)) {
        return;  // tethers off entirely; attachments run in their own pass
    }
    const f32 stiffness = Clamp01(params_.tetherStiffness);
    const f32 oneMinus = 1.0f - stiffness;
    for (i32 i = pinnedCount_; i < ParticleCount(); ++i) {
        Particle& p = particles_[i];
        if (p.tetherRoot < 0) {
            continue;
        }
        const Vec4 rootPos = particles_[p.tetherRoot].position;
        const Vec4 toParticle = p.position - rootPos;
        const f32 distSq = Dot3(toParticle, toParticle);
        if (!(distSq > kClothTiny)) {
            continue;
        }
        const f32 maxDist = p.tetherRestLength * worldScale_;
        if (!(maxDist * maxDist < distSq)) {
            continue;
        }
        // Pull back to the sphere of radius maxDist about the root; small violations get the
        // soft ramp min(2*|corr|, 1) even at low stiffness.
        const Vec4 correction =
            (rootPos - p.position) + (toParticle * maxDist) * Rsqrt(distSq);
        const f32 len = std::sqrt(Dot3(correction, correction));
        const f32 blend = std::min(len + len, 1.0f) * oneMinus + stiffness;
        p.position = p.position + correction * blend;
    }
}

void Cloth::CollideCapsules() {
    // Pass order: WORLD capsules first with radius scale 1, then the local records with
    // the cloth's world scale. Broadphase: the capsule's current-pose AABB from RAW
    // record values (the radius scale is absorbed by the margin), fattened by twice the
    // largest scaled rest edge.
    const f32 margin2 = 2.0f * maxScaledRestEdge_;
    const auto sweep = [&](const std::vector<Capsule>& list, f32 radiusScale) {
        for (const Capsule& c : list) {
            const f32 rMax = std::max(c.radii.z, c.radii.y);
            const f32 rExt = std::max(c.radius1 * rMax, c.radius0 * rMax);
            const Vec4 axisW =
                Rotate(c.rotation, constants::kUnitX) * (c.fullLength * c.radii.x * 0.5f);
            const Vec4 endA = c.position - axisW;
            const Vec4 endB = c.position + axisW;
            const Vec4 lo{std::min(endA.x, endB.x) - rExt, std::min(endA.y, endB.y) - rExt,
                          std::min(endA.z, endB.z) - rExt, 0.0f};
            const Vec4 hi{std::max(endB.x, endA.x) + rExt, std::max(endB.y, endA.y) + rExt,
                          std::max(endB.z, endA.z) + rExt, 0.0f};
            for (i32 i = pinnedCount_; i < ParticleCount(); ++i) {
                const Vec4& p = particles_[i].position;
                if (p.x + margin2 - lo.x < 0.0f || p.y + margin2 - lo.y < 0.0f ||
                    p.z + margin2 - lo.z < 0.0f || margin2 - p.x + hi.x < 0.0f ||
                    margin2 - p.y + hi.y < 0.0f || margin2 - p.z + hi.z < 0.0f) {
                    continue;
                }
                CollideParticleCapsule(i, c, radiusScale);
            }
        }
    };
    sweep(worldCapsules_, 1.0f);
    sweep(capsules_, worldScale_);
}

void Cloth::CollideParticleCapsule(i32 i, const Capsule& c, f32 radiusScale) {
    // The PROXY particle drives the closest-feature query and the normal; the particle
    // itself drives the penetration test and takes the push. Every guard failure drops
    // the contact without touching the position.
    Particle& p = particles_[i];
    const Vec4 pos = p.position;
    const Vec4 sample = hasSelfCollision_ ? pos : particles_[p.proxy].position;
    const f32 s = radiusScale;
    const f32 r0 = c.radius0 * s;
    const f32 halfL = (c.fullLength * s) * 0.5f;
    const auto toUnit = [&](const Vec4& x) {
        return InverseRotate(c.rotation, x - c.position) * c.inverseRadii;
    };
    const Vec4 uS = toUnit(sample);
    const Vec4 uP = toUnit(pos);

    Vec4 nHat{};
    f32 pen = 0.0f;
    if (c.featureType == 2) {
        // Finite segment along local x, radius r0.
        const Vec4 ax{std::max(-halfL, std::min(uS.x, halfL)), 0.0f, 0.0f, 0.0f};
        const Vec4 d = uS - ax;
        const f32 dSq = Dot3(d, d);
        if (!(dSq > kClothTiny)) {
            return;
        }
        nHat = d * Rsqrt(dSq);
        pen = Dot3(uP - ax, nHat) - r0;
    } else if (c.featureType == 1) {
        // Sphere at the origin.
        const f32 dSq = Dot3(uS, uS);
        if (!(dSq > kClothTiny)) {
            return;
        }
        nHat = uS * Rsqrt(dSq);
        pen = Dot3(uP, nHat) - r0;
    } else {
        // Tapered capsule: sphere(-L/2, r0), cone lateral surface, sphere(+L/2, r1). The
        // region selector walks the lateral tangent from the -x surface point; the record
        // carries the unit tilt pair -- the cone normal is NOT re-normalised.
        Vec4 radial = uS;
        radial.x = 0.0f;
        const f32 rSq = Dot3(radial, radial);
        const Vec4 rHat = (rSq > kClothTiny) ? radial * Rsqrt(rSq) : constants::kUnitY;
        const Vec4 coneN{c.taper.y, rHat.y * c.taper.z, rHat.z * c.taper.z, 0.0f};
        const Vec4 coneT{c.taper.z, -rHat.y * c.taper.y, -rHat.z * c.taper.y, 0.0f};
        const Vec4 endOff = constants::kUnitX * halfL;
        const Vec4 surfPt = coneN * r0 - endOff;
        const f32 h = Dot3(uS - surfPt, coneT);
        if (h < 0.0f) {
            const Vec4 d = uS + endOff;
            const f32 dSq = Dot3(d, d);
            if (!(dSq > kClothTiny)) {
                return;
            }
            nHat = d * Rsqrt(dSq);
            pen = Dot3(uP + endOff, nHat) - r0;
        } else if (!(c.taper.x < h)) {
            nHat = coneN;
            pen = Dot3(uP - surfPt, coneN);
        } else {
            const Vec4 d = uS - endOff;
            const f32 dSq = Dot3(d, d);
            if (!(dSq > kClothTiny)) {
                return;
            }
            nHat = d * Rsqrt(dSq);
            pen = Dot3(uP - endOff, nHat) - (s * c.radius1);
        }
    }
    if (!(pen < 0.0f)) {
        return;
    }

    // Shared tail: pushout along the inverse-transpose world normal, depth accumulation,
    // then the same previous-pose friction the half-space path uses.
    const f32 depth = Dot3(-(nHat * pen), nHat);
    const Vec4 cpU = uP + nHat * (c.taper.w * depth);
    const Vec4 cpL = cpU * c.radii;
    const Vec4 cpW = c.position + Rotate(c.rotation, cpL);
    const Vec4 nUnscaled = Rotate(c.rotation, nHat * c.inverseRadii);
    const Vec4 nW = nUnscaled * Rsqrt(Dot3(nUnscaled, nUnscaled));
    p.penetrationDepth += Dot3(cpW - pos, nW);
    const Vec4 slip =
        ((Rotate(c.prevRotation, cpL) + c.prevPosition) - cpW) + (cpW - p.integrationStart);
    Vec4 tangential = slip - nW * Dot3(slip, nW);
    const f32 tanLen = std::sqrt(Dot3(tangential, tangential));
    const f32 frictionLimit = p.penetrationDepth * params_.friction;
    if (frictionLimit <= tanLen) {
        tangential = tangential * std::min(GuardedRcp(tanLen) * frictionLimit, 1.0f);
    }
    p.position = cpW - tangential;
}

void Cloth::CollidePlanes() {
    // World half-spaces first, then the local records, through one sweep -- storing the
    // position on contact only and storing it unconditionally are the same observable, so
    // both families share the contact-only form.
    const f32 mu = params_.friction;
    const auto sweep = [&](const Plane& pl) {
        for (i32 i = pinnedCount_; i < ParticleCount(); ++i) {
            Particle& p = particles_[i];
            const f32 dist = Dot3(p.position - pl.position, pl.axis);
            if (!(dist < 0.0f)) {
                continue;
            }
            const f32 push = dist * pl.pushScale;
            const Vec4 surface = p.position - pl.axis * push;
            p.penetrationDepth -= push;

            // Friction works on the tangential slip of the particle against the contact
            // point's own motion (the collider's previous pose re-projects it), clamped by
            // mu times the accumulated penetration depth: hold below the limit, slide at it.
            const Vec4 localPt = InverseRotate(pl.rotation, surface - pl.position);
            const Vec4 slip = ((pl.prevPosition - surface) + Rotate(pl.prevRotation, localPt)) +
                              (surface - p.integrationStart);
            Vec4 tangential = slip - pl.axis * Dot3(slip, pl.axis);
            const f32 tanLen = std::sqrt(Dot3(tangential, tangential));
            const f32 frictionLimit = p.penetrationDepth * mu;
            if (frictionLimit <= tanLen) {
                tangential = tangential * std::min(GuardedRcp(tanLen) * frictionLimit, 1.0f);
            }
            p.position = surface - tangential;
        }
    };
    for (const Plane& pl : worldPlanes_) {
        sweep(pl);
    }
    for (const Plane& pl : planes_) {
        sweep(pl);
    }
}

void Cloth::RestPosePull() {
    const f32 stiffness = params_.restPoseStiffness;
    if (!(stiffness >= 1.1920929e-7f)) {
        return;
    }
    const f32 s = worldScale_;
    const bool anchored =
        !anchorMap_.empty() && frameBind_ != nullptr && frameAnim_ != nullptr;
    for (i32 i = pinnedCount_; i < ParticleCount(); ++i) {
        Particle& p = particles_[i];
        // Anchored: pull toward the 4-bone skinned rest position (both arrays straight
        // from the frame, raw slots); otherwise toward the rigidly transformed rest.
        const Vec4 local =
            anchored ? SkinPoint(p, p.restPosition, frameBind_, frameAnim_, true) * s
                     : p.restPosition * s;
        const Vec4 target = Rotate(worldCur_.rotation, local) + worldCur_.position;
        p.position = p.position + (target - p.position) * stiffness;
    }
}

void Cloth::RecomputeVelocities(f32 dt) {
    const f32 invDt = GuardedRcp(dt);
    for (i32 i = pinnedCount_; i < ParticleCount(); ++i) {
        Particle& p = particles_[i];
        p.velocity = (p.position - p.integrationStart) * invDt;
        p.auxVelocity = (p.position - p.prevPosition) * invDt;
    }
}

Vec4 Cloth::SkinPoint(const Particle& p, const Vec4& local, const ClothAnchor* bind,
                      const ClothAnchor* anim, bool translate) const {
    // The 4-slot (bind^-1 -> anim) skin shared by the attachment and rest-pull passes: raw
    // slot indexing into BOTH frame arrays, weights blended in slot order. `translate`
    // false is the normal variant -- rotations only, no bind offset, no anim position.
    const auto bone = [&](i32 k) {
        const ClothAnchor& b = bind[p.anchorSlots[k]];
        const ClothAnchor& a = anim[p.anchorSlots[k]];
        if (translate) {
            return Rotate(a.rotation, InverseRotate(b.rotation, local - b.position)) +
                   a.position;
        }
        return Rotate(a.rotation, InverseRotate(b.rotation, local));
    };
    Vec4 acc = bone(0) * p.anchorWeights.x;
    acc = acc + bone(1) * p.anchorWeights.y;
    acc = acc + bone(2) * p.anchorWeights.z;
    acc = acc + bone(3) * p.anchorWeights.w;
    return acc;
}

void Cloth::SolveAttachments() {
    // A one-sided plane keeping particles in front of the skinned rest surface. The offset
    // is the tether LUT through the world scale; the direction is the skinned rest normal --
    // normalised (and guarded) only on the skinned path, used RAW on the unskinned one. The
    // asymmetry is deliberate: normalising the raw path changes every unanchored cloth.
    if (!params_.attachmentsEnabled) {
        return;
    }
    const f32 k = Clamp01(params_.attachmentStiffness);
    const bool anchored =
        !anchorMap_.empty() && frameBind_ != nullptr && frameAnim_ != nullptr;
    // The distance folds the offset into the X lane: ((z + y) + (x - offset)).
    const auto planeDistance = [](const Vec4& v, const Vec4& dir, f32 offset) {
        const Vec4 m = v * dir;
        return (m.z + m.y) + (m.x - offset);
    };
    for (i32 i = pinnedCount_; i < ParticleCount(); ++i) {
        Particle& p = particles_[i];
        const f32 offset = worldScale_ * tetherLut_[p.lutIndex];
        if (anchored) {
            const Vec4 targetLocal =
                SkinPoint(p, p.restPosition, frameBind_, frameAnim_, true) * worldScale_;
            const Vec4 target =
                Rotate(worldCur_.rotation, targetLocal) + worldCur_.position;
            p.attachTarget = target;
            const Vec4 nLocal = SkinPoint(p, p.skinnedRestNormal, frameBind_, frameAnim_,
                                          false);
            const Vec4 dir = Rotate(worldCur_.rotation, nLocal);
            const f32 dSq = Dot3(dir, dir);
            if (dSq > kClothTiny) {
                const Vec4 dirHat = dir * Rsqrt(dSq);
                const f32 d = planeDistance(p.position - target, dirHat, offset);
                p.position = p.position - dirHat * (std::min(d, 0.0f) * k);
                p.attachDir = dirHat;  // NOT written when the guard fails
            }
        } else {
            const Vec4 target =
                Rotate(worldCur_.rotation, p.restPosition * worldScale_) + worldCur_.position;
            p.attachTarget = target;
            const Vec4 dir = Rotate(worldCur_.rotation, p.restNormal);  // unnormalised
            p.attachDir = dir;
            const f32 d = planeDistance(p.position - target, dir, offset);
            p.position = p.position - dir * (std::min(d, 0.0f) * k);
        }
    }
}

void Cloth::BuildOutputFrames() {
    // One pass refreshes the cached triangle normals AND accumulates the area-weighted
    // particle normals; the frame build then Gram-Schmidts a basis toward the reference
    // particle and composes the authored reference rows through it. The accumulator is a
    // field of its own rather than an alias of the Z output row, so reading it back stays
    // meaningful after the rows are rebuilt.
    for (Particle& p : particles_) {
        p.normalAccum = {};
    }
    for (Triangle& t : triangles_) {
        const Vec4 e01 = particles_[t.b].position - particles_[t.a].position;
        const Vec4 e02 = particles_[t.c].position - particles_[t.a].position;
        const Vec4 n = Cross3(e01, e02);
        const f32 nSq = Dot3(n, n);
        // Degenerate triangles keep LAST step's cached normal -- but their raw tiny cross
        // is still what gets accumulated, deliberately.
        Vec4 nv = n;
        if (nSq > kClothTiny) {
            nv = n * Rsqrt(nSq);
            t.normal = nv;
        }
        const Vec4 w = nv * t.area;
        particles_[t.a].normalAccum = particles_[t.a].normalAccum + w;
        particles_[t.b].normalAccum = particles_[t.b].normalAccum + w;
        particles_[t.c].normalAccum = particles_[t.c].normalAccum + w;
    }
    for (Particle& p : particles_) {
        Vec4 x = constants::kUnitX;
        Vec4 y = constants::kUnitY;
        Vec4 z = constants::kUnitZ;
        const f32 aSq = Dot3(p.normalAccum, p.normalAccum);
        if (aSq > kClothTiny && p.frameReference >= 0) {
            z = p.normalAccum * Rsqrt(aSq);
            const Vec4 toRef = particles_[p.frameReference].position - p.position;
            const Vec4 t = toRef - z * Dot3(z, toRef);
            const f32 tSq = Dot3(t, t);
            if (tSq > kClothTiny) {
                x = t * Rsqrt(tSq);
                y = Cross3(z, x);
            }
        }
        const Vec4* r = p.refRows;
        p.outputRows[0] = z * r[0].z + (y * r[0].y + x * r[0].x);
        p.outputRows[1] = z * r[1].z + (y * r[1].y + x * r[1].x);
        p.outputRows[2] = z * r[2].z + (y * r[2].y + x * r[2].x);
        p.outputRows[3] = (z * r[3].z + y * r[3].y) + (x * r[3].x + p.position);
    }
}

bool Cloth::SweptSphereQuery(const Vec4& start, const Vec4& end, f32 maxT,
                             ClothSweepHit& out) const {
    // Despite the name, a segment cast -- see the header. The comparisons are spelled so
    // NaN lanes fall the intended way: a degenerate cloth PROCEEDS through the sign and
    // parallel gates (their rejects are compares that NaN breaks) and dies at the t-window
    // instead, so the gate order below is not free to rearrange.
    const Vec4 d = end - start;
    f32 clamp = maxT;
    bool any = false;
    for (usize k = 0; k < triangles_.size(); ++k) {
        const Triangle& tr = triangles_[k];
        const Vec4 p0 = particles_[tr.a].position;
        const Vec4 p1 = particles_[tr.b].position;
        const Vec4 p2 = particles_[tr.c].position;
        const Vec4 f0 = p0 - start;
        const Vec4 f1 = p1 - start;
        const Vec4 f2 = p2 - start;
        // The segment passes inside the triangle iff the three signed volumes agree in
        // sign; the pairwise-product min is the NaN-tolerant spelling of "agree".
        const f32 u = Dot3(Cross3(f2, f1), d);
        const f32 v = Dot3(Cross3(f0, f2), d);
        const f32 w = Dot3(Cross3(f1, f0), d);
        if (std::min(std::min(v * w, u * v), w * u) < 0.0f) {
            continue;
        }
        const Vec4 n = Cross3(p1 - p0, p2 - p0);
        const f32 nd = Dot3(n, d);
        if (std::max(nd, -nd) < 1.1920929e-7f) {
            continue;  // parallel to the face plane
        }
        // t = dot(p0 - start, n) / dot(n, d), through the form-E reciprocal of the
        // NEGATED denominator -- an association one ulp off a plain divide, and kept.
        const f32 t = NegRcpBend(-nd) * Dot3(f0, n);
        if (!(t >= 0.0f) || !(t <= clamp)) {
            continue;
        }
        out.point = d * t + start;
        out.normal = n * Rsqrt(Dot3(n, n));
        out.t = t;
        out.triangle = static_cast<i32>(k);
        out.triangleCount = static_cast<i32>(triangles_.size());
        clamp = t;  // progressive: later triangles must beat this hit
        any = true;
    }
    return any;
}

void Cloth::SetScale(const ClothDef& source, f32 newScale) {
    // Rebuild the scaled lanes from the immutable source, re-transform every particle by
    // the scale ratio, re-derive the mass products, and force the next PreSolve hard.
    // Colliders and rest positions are NOT touched. The bend records carry the deliberate
    // Create/SetScale disagreement the file comment names: the rest angle picks up a factor
    // of s here (Create leaves it alone), and the clamps go back to their RAW authored
    // values (Create squared the scale onto them).
    const f32 oldScale = worldScale_;
    if (std::abs(newScale - oldScale) < 1.1920929e-7f) {
        return;
    }
    const f32 s = std::max(newScale, 0.01f);
    worldScale_ = s;

    for (usize k = 0; k < triangles_.size(); ++k) {
        triangles_[k].area = source.triangles[k].area * (s * s);
    }
    maxScaledRestEdge_ = 0.0f;
    for (usize k = 0; k < edges_.size(); ++k) {
        const ClothEdgeDef& in = source.edges[k];
        Edge& e = edges_[k];
        e.restLength = in.restLength * s;
        e.blend1 = in.stiffnessBlend1;
        e.blend2 = in.stiffnessBlend2;
        e.selfMask = in.selfCollisionMask;
        maxScaledRestEdge_ = std::max(maxScaledRestEdge_, e.restLength);
    }
    for (usize k = 0; k < bends_.size(); ++k) {
        const ClothBendDef& in = source.bends[k];
        Bend& b = bends_[k];
        b.restAngle = in.restAngle * s;
        b.selfMask = in.selfCollisionMask;
        b.clampA = in.clampA;
        b.clampB = in.clampB;
    }

    const f32 ratio = s / oldScale;
    for (Particle& p : particles_) {  // ALL particles, pinned included
        p.position =
            Rotate(worldCur_.rotation,
                   InverseRotate(worldPrev_.rotation, p.position - worldPrev_.position) *
                       ratio) +
            worldCur_.position;
    }

    // The particle-parameter derivation again, over the new scale. Masses zeroed by
    // EnableSelfCollision STAY zero -- it reads the live authored mass, not the def.
    const f32 invMassScale = (1.0f / params_.massMultiplier) * (1.0f / (s * s));
    for (i32 i = pinnedCount_; i < ParticleCount(); ++i) {
        particles_[i].scaledInverseMass = particles_[i].authoredInverseMass * invMassScale;
    }
    for (Edge& e : edges_) {
        const f32 sum = particles_[e.a].scaledInverseMass + particles_[e.b].scaledInverseMass;
        const f32 w = sum > kClothTiny ? 1.0f / sum : 0.0f;
        e.weightA = particles_[e.a].scaledInverseMass * w;
        e.weightB = particles_[e.b].scaledInverseMass * w;
    }
    for (Bend& b : bends_) {
        const u16 ends[4] = {b.e0, b.e1, b.e2, b.e3};
        for (i32 j = 0; j < 4; ++j) {
            b.invMass[j] = particles_[ends[j]].scaledInverseMass;
        }
        const f32 sum = b.invMass[0] + b.invMass[1];
        const f32 w = sum > kClothTiny ? 1.0f / sum : 0.0f;
        b.weightA = b.invMass[0] * w;
        b.weightB = b.invMass[1] * w;
    }
    for (usize i = 0; i < tetherLut_.size(); ++i) {
        tetherLut_[i] = std::pow(static_cast<f32>(i) * invTetherLutCount_,
                                 params_.tetherExponent) *
                        params_.tetherScale;
    }

    worldPrev_ = worldCur_;
    forceTeleport_ = true;
}

}  // namespace snowball
