//===----------------------------------------------------------------------===//
// snowball/cloth.h -- the soft-body solver: cloth as position-based dynamics.
//
// A particle system that lives beside the rigid-body engine and shares nothing with it: no
// bodies, no islands, no scene. One Cloth is one independent simulation, stepped by its host
// once per frame with that frame's world transform, bone anchors and staged colliders. Every
// scaled quantity runs through one uniform world scale -- applied to the record lanes at
// Create/SetScale and to the colliders at advection/narrowphase time.
//
// A naming warning for readers of the types below: the "sphere" world collider has never
// been a sphere -- it is a half-space -- and the "swept sphere" query is a segment cast.
// Both names are API surface and stay; the behaviour documented on each is what counts.
//
// Step order, which is the contract: transform-group teleport handling -> wind impulse (only
// on non-teleport frames) -> integrate (v = v*exp(-damping*dt) + g*dt) -> distance sweep ->
// bend sweep -> distance sweep AGAIN -> tethers -> colliders -> rest-pose pull -> velocity
// recompute -> per-particle output frames.
//===----------------------------------------------------------------------===//
#pragma once

#include <vector>

#include "snowball/common_types.h"
#include "snowball/transform.h"
#include "snowball/vec.h"

namespace snowball {

/// An SQT anchor record -- the format shared by the def's bind pose and the frame's
/// animated bones (quat xyzw + position).
struct ClothAnchor {
    Vec4 rotation{constants::kQuatIdentity};
    Vec4 position{};
};

/// One particle of the authored cloth. Particles [0, pinnedCount) of the def are pinned and
/// driven by the world transform; the rest are dynamic. Positions are LOCAL and unscaled --
/// Create applies the def's world transform and uniform scale.
struct ClothParticleDef {
    Vec4 restPosition{};
    Vec4 restNormal{constants::kUnitY};    // the UNSKINNED attachment direction
    Vec4 skinnedRestNormal{};              // rest normal for the SKINNED attachment path
    f32 inverseMass{1.0f};       // authored; 0 pins the mass without pinning the particle
    f32 tetherRestLength{0.0f};  // scaled by worldScale at solve time
    i16 tetherRoot{-1};          // particle whose position bounds this one; -1 = none.
                                 // With anchors it doubles as the particle whose anchor
                                 // table the group advection borrows.
    i16 lutIndex{0};             // hop count from the pinned boundary (tether LUT index)
    i16 transformGroup{-1};      // teleport-detection group; -1 = none
    i16 frameReference{-1};      // tangent reference particle for the output frame; -1 = none
    i32 collisionProxy{-1};      // particle sampled by the capsule query; -1 = itself
    bool selfCollision{false};   // freezes this particle when EnableSelfCollision fires
    u16 anchorSlots[4]{};        // raw model bone slots for the 4-way skin
    Vec4 anchorWeights{};        // skin weights over the four slots
    // The authoring-built reference matrix rows the output frame composes through the
    // particle basis; identity rows leave the basis as the output.
    Vec4 refRow0{constants::kUnitX};
    Vec4 refRow1{constants::kUnitY};
    Vec4 refRow2{constants::kUnitZ};
    Vec4 refRow3{};
};

/// A stretch constraint between two particles. `restLength` is unscaled; the two blend lanes
/// select a mix of the three global distance stiffnesses:
/// stiffness = (1-b1-b2)*s0 + b1*s1 + b2*s2.
struct ClothEdgeDef {
    u16 a{0};
    u16 b{0};
    f32 restLength{0.0f};
    f32 stiffnessBlend1{0.0f};
    f32 stiffnessBlend2{0.0f};
    f32 selfCollisionMask{0.0f};
};

/// A bend (dihedral) constraint over four particles. Endpoints are stored in baked-record
/// order; the solve reads them back as (p1, p2, p0, p3). Create scales `clampA`/`clampB` by
/// worldScale squared -- and SetScale deliberately disagrees, rescaling only the rest-angle
/// lane (see SetScale for why the disagreement is contract).
struct ClothBendDef {
    u16 e0{0};
    u16 e1{0};
    u16 e2{0};
    u16 e3{0};
    f32 restAngle{0.0f};         // killed when params.killBendRestAngle
    f32 selfCollisionMask{0.0f};
    f32 clampA{0.0f};
    f32 clampB{0.0f};
};

/// A triangle: the aerodynamics quantum and the source of particle normals. `area` is the
/// authored rest area, unscaled (Create applies worldScale squared).
struct ClothTriangleDef {
    u16 a{0};
    u16 b{0};
    u16 c{0};
    f32 area{0.0f};
    bool selfCollision{false};
};

/// A capsule collider record. The shape is a tapered capsule along local +x:
/// sphere(-L/2, radius0), the cone lateral surface, sphere(+L/2, radius1) -- degenerate
/// forms selected by `featureType` (1 = sphere at the origin, 2 = plain segment of radius0,
/// anything else = the full tapered shape). `radii`/`inverseRadii` are the per-axis forward
/// and inverse radii of the unit-space transform the narrowphase works in. The local pose
/// rides its anchor bone when `anchor` >= 0, and the bare cloth world transform otherwise.
struct ClothCapsuleDef {
    Vec4 localRotation{constants::kQuatIdentity};
    Vec4 localPosition{};        // UNSCALED -- worldScale is applied at advection
    Vec4 radii{1.0f, 1.0f, 1.0f, 0.0f};         // unit -> local map, AABB extents
    Vec4 inverseRadii{1.0f, 1.0f, 1.0f, 0.0f};  // local -> unit map
    f32 radius0{0.0f};                          // -x end / sphere / segment radius
    f32 radius1{0.0f};                          // +x end (taper)
    f32 fullLength{0.0f};                       // FULL length (code halves it)
    Vec4 taper{};       // {region extent, tilt axial, tilt radial, pushout scale}
    u16 featureType{2};                         // narrowphase dispatch selector
    i16 anchor{-1};     // raw model bone index; -1 = world/model-driven
};

/// The capsule classifier's input: a raw pose + per-axis scale + two end radii and a
/// length, turned into a classified ClothCapsuleDef record.
struct ClothCapsuleDesc {
    Vec4 localRotation{constants::kQuatIdentity};
    Vec4 localPosition{};
    Vec4 scale{constants::kOne};   // per-axis, clamped to [0.5, 2.0]
    f32 radius0{0.0f};
    f32 radius1{0.0f};
    f32 length{0.0f};
    f32 pushScale{1.0f};
    i16 anchor{0};
};

/// Classify a capsule desc into a record. Returns false for a degenerate desc (radius <=
/// 0.005 or negative length) -- the record is still fully written, with featureType 0, on
/// purpose: the world staging KEEPS such records, and type 0 dispatches into the tapered
/// narrowphase branch rather than being rejected.
bool ClassifyClothCapsule(const ClothCapsuleDesc& desc, ClothCapsuleDef& out);

/// A half-space collider: particles are kept on the positive side of the plane through
/// `localPosition` with normal rotate(q, +z). The local pose rides its anchor bone when
/// `anchor` >= 0, like the capsules.
struct ClothPlaneDef {
    Vec4 localRotation{constants::kQuatIdentity};
    Vec4 localPosition{};
    f32 pushScale{1.0f};
    i16 anchor{-1};     // raw model bone index; -1 = world/model-driven
};

/// One world-driven capsule handed in per frame: both world poses plus the raw radii. Step
/// classifies it like a def capsule and collides it with radius scale 1 -- world capsules
/// do not pick up the cloth's uniform scale.
struct ClothWorldCapsule {
    Vec4 prevRotation{constants::kQuatIdentity};
    Vec4 prevPosition{};
    Vec4 rotation{constants::kQuatIdentity};
    Vec4 position{};
    f32 radius0{0.0f};
    f32 radius1{0.0f};
    f32 fullLength{0.0f};
};

/// One world-driven half-space per frame (both world poses; the plane normal is
/// rotate(rotation, +z)). Despite the name there is no sphere here -- see the file comment.
/// Hosts historically never write `pushScale` when staging these records, so the member
/// default is load-bearing rather than cosmetic: it is what actually collides.
struct ClothWorldSphere {
    Vec4 prevRotation{constants::kQuatIdentity};
    Vec4 prevPosition{};
    Vec4 rotation{constants::kQuatIdentity};
    Vec4 position{};
    f32 pushScale{1.0f};
};

/// Tuning block, one per cloth.
struct ClothParams {
    Vec4 gravity{0.0f, 0.0f, -10.0f};
    Vec4 windVelocity{};

    /// Group pull toward the world transform below the teleport threshold.
    f32 worldFollowStiffness{1.0f};
    f32 distanceStiffness[3]{1.0f, 1.0f, 1.0f};
    f32 bendStiffness{1.0f};
    f32 radialImpulseScale{0.0f};

    /// The wind pass: flow = aeroVelocityGain/3 * (2*vAux[b] + vAux[a]) - windVelocity -
    /// aeroNoiseGain * windNoise. Pass runs only when either gain is positive.
    f32 aeroNoiseGain{0.0f};
    f32 aeroVelocityGain{0.0f};
    f32 liftFactor{0.0f};        // scales the (n x v) x v lift term

    f32 damping{0.0f};           // per second; velocity *= exp(-damping*dt)
    f32 friction{0.0f};          // collider Coulomb mu
    f32 tetherScale{1.0f};
    f32 tetherExponent{1.0f};
    f32 attachmentStiffness{0.0f};
    f32 tetherStiffness{0.0f};
    f32 massMultiplier{1.0f};    // divides every inverse mass

    bool killBendRestAngle{false};  // zeroes every bend rest angle at solve time
    bool attachmentsEnabled{false}; // gates the attachment pass
    bool hasColliders{false};

    f32 windImpulseScale{0.0f};  // scales the whole wind impulse
    f32 restPoseStiffness{0.0f}; // pull toward the (skinned) rest pose
};

struct ClothDef {
    Transform world{};
    f32 worldScale{1.0f};        // clamped to >= 0.01 at Create
    u32 pinnedCount{0};
    u32 transformGroupCount{0};
    u32 tetherLutCount{0};       // the LUT holds tetherLutCount + 1 entries
    ClothParams params{};
    std::vector<ClothParticleDef> particles;
    std::vector<ClothEdgeDef> edges;
    std::vector<ClothBendDef> bends;
    std::vector<ClothTriangleDef> triangles;
    std::vector<ClothCapsuleDef> capsules;
    std::vector<ClothPlaneDef> planes;
    // The anchor tables (empty = the cloth is unanchored and every skinned path falls back
    // to the bare world transform). `bindPose` is indexed by RAW model bone slot;
    // `anchorMap` maps slot -> anchor state index or -1.
    std::vector<ClothAnchor> bindPose;
    std::vector<i16> anchorMap;
    u32 anchorStateCount{0};
};

/// Per-frame input, authored by the host before every step. The anchor arrays are indexed
/// by RAW model bone slot and must stay alive for the duration of the Step call -- the
/// skinned attachment and rest-pull passes read them mid-solve.
struct ClothFrame {
    f32 dt{1.0f / 60.0f};
    Transform world{};
    Vec4 windNoise{};            // sampled by the host, written before each step
    const ClothAnchor* bind{nullptr};   // bind bone SQTs
    const ClothAnchor* anim{nullptr};   // this frame's animated bone SQTs
    const ClothWorldCapsule* worldCapsules{nullptr};   // world-driven capsules, staged raw
    i32 worldCapsuleCount{0};
    const ClothWorldSphere* worldSpheres{nullptr};     // world-driven half-spaces
    i32 worldSphereCount{0};
};

/// SweptSphereQuery's hit record.
struct ClothSweepHit {
    Vec4 point{};        // the segment point at the hit
    Vec4 normal{};       // unit triangle normal, straight from cross(e01, e02) --
                         // NOT flipped toward the segment
    f32 t{};             // the segment fraction of the hit
    i32 triangle{-1};    // index of the winning triangle
    i32 triangleCount{0};  // total triangles scanned
};

class Cloth {
public:
    void Create(const ClothDef& def);
    void Step(const ClothFrame& frame);

    /// Despite the name, a SEGMENT cast -- no radius ever enters: each triangle runs an
    /// analytic ray test (signed-volume sign gate, FLT_EPSILON parallel guard, t through
    /// the form-E reciprocal). Hits accept on 0 <= t <= the clamp, which starts at `maxT`
    /// and tightens to each accepted t -- so the result is the nearest hit. Every triangle
    /// is scanned (no broadphase, no degenerate skip), against CURRENT particle positions;
    /// `out` is written on hits only. An external query API, like ApplyRadialImpulse --
    /// nothing inside the solver calls it.
    bool SweptSphereQuery(const Vec4& start, const Vec4& end, f32 maxT,
                          ClothSweepHit& out) const;

    /// A one-way switch that zeroes the masses and velocities of every flagged particle and
    /// turns the self-collision mode on (frozen proxies + masked constraints; there is no
    /// extra narrowphase pass anywhere in it).
    void EnableSelfCollision();

    /// Outward velocity kick on dynamic particles within `radius` of `center` -- no falloff,
    /// magnitude = scaledInverseMass * radialImpulseScale * strength capped at 0.25 per
    /// particle. Returns the largest PRE-clamp magnitude seen on a self-collision-flagged
    /// particle (0 when the scale parameter is zero).
    f32 ApplyRadialImpulse(const Vec4& center, f32 radius, f32 strength);

    /// Rescale the cloth to a new uniform world scale, rebuilding the scaled record lanes
    /// from the immutable `source` (which must be the def Create consumed). No-op when the
    /// scale moved less than FLT_EPSILON; the next PreSolve is forced hard. Collider records
    /// are NOT touched -- the scale reaches them at advection/narrowphase time. Create and
    /// SetScale deliberately disagree on the bend lanes: SetScale multiplies the bend REST
    /// ANGLE by the scale and restores the clamps raw, where Create leaves the angle alone
    /// and squares the clamps. Both sides are contract -- harmonising them changes how a
    /// rescaled cloth bends.
    void SetScale(const ClothDef& source, f32 newScale);

    i32 ParticleCount() const { return static_cast<i32>(particles_.size()); }
    const Vec4& Position(i32 i) const { return particles_[i].position; }
    const Vec4& Velocity(i32 i) const { return particles_[i].velocity; }
    const Vec4& AuxVelocity(i32 i) const { return particles_[i].auxVelocity; }
    /// Output-frame readback: rows 0..2 = the composed X/Y/Z axes, row 3 = the origin.
    /// Rebuilt at the end of every Step and once at Create.
    const Vec4& OutputRow(i32 i, i32 row) const { return particles_[i].outputRows[row]; }

    // Derived-state readback: the quantities Create computes from the def.
    f32 WorldScale() const { return worldScale_; }
    f32 MaxScaledRestEdge() const { return maxScaledRestEdge_; }
    f32 ScaledInverseMass(i32 i) const { return particles_[i].scaledInverseMass; }
    f32 EdgeWeightA(i32 k) const { return edges_[k].weightA; }
    f32 EdgeWeightB(i32 k) const { return edges_[k].weightB; }
    f32 TetherLut(i32 i) const { return tetherLut_[i]; }
    f32 BendWeightA(i32 k) const { return bends_[k].weightA; }
    f32 BendWeightB(i32 k) const { return bends_[k].weightB; }

private:
    struct Particle {
        Vec4 position{};
        Vec4 prevPosition{};       // at step start; teleports rewrite it
        Vec4 integrationStart{};   // just before velocities integrate
        Vec4 velocity{};
        Vec4 auxVelocity{};        // (position - prevPosition) / dt
        Vec4 restPosition{};       // local, unscaled
        Vec4 restNormal{};         // the unskinned attachment direction
        Vec4 skinnedRestNormal{};  // the skinned attachment direction source
        Vec4 normalAccum{};        // area-weighted normal accumulator for the output sweep
        Vec4 outputRows[4]{};      // composed X/Y/Z rows + origin
        Vec4 refRows[4]{};         // the authoring-built reference matrix
        Vec4 attachTarget{};       // cached attachment target (world)
        Vec4 attachDir{};          // cached attachment direction
        Vec4 anchorWeights{};      // skin weights over the four slots
        u16 anchorSlots[4]{};      // raw model bone slots
        f32 penetrationDepth{};    // per-step collider accumulator
        f32 authoredInverseMass{};
        f32 scaledInverseMass{};   // authored / (massMultiplier * scale^2)
        f32 tetherRestLength{};
        i16 tetherRoot{};
        i16 lutIndex{};
        i16 group{};
        i16 frameReference{};
        i32 proxy{};               // capsule-query sample particle (itself unless authored)
        bool selfFlag{};           // authored self-collision flag
    };
    struct Edge {
        u16 a{}, b{};
        f32 restLength{};          // scaled
        f32 blend1{}, blend2{}, selfMask{};
        f32 weightA{}, weightB{};  // invMass_i / (invMass_a + invMass_b), 0 when both pinned
    };
    struct Bend {
        u16 e0{}, e1{}, e2{}, e3{};  // baked-record order; the solve's (p1, p2, p0, p3)
        f32 invMass[4]{};            // the four endpoints' scaled inverse masses
        f32 restAngle{}, selfMask{};
        f32 clampA{}, clampB{};      // scaled by worldScale^2 at Create
        f32 weightA{}, weightB{};    // the e0/e1 pair weights, both-pinned guard
    };
    struct Triangle {
        u16 a{}, b{}, c{};
        f32 area{};                // scaled by worldScale^2
        Vec4 normal{};             // cached face normal, refreshed every step
        bool selfCollision{};
    };
    struct Capsule {
        Vec4 localRotation{}, localPosition{};
        Vec4 prevRotation{}, prevPosition{};
        Vec4 rotation{}, position{};
        Vec4 radii{}, inverseRadii{};
        Vec4 taper{};
        f32 radius0{}, radius1{}, fullLength{};
        u16 featureType{};
        i16 anchor{-1};            // raw model bone; -1 = world/model-driven
    };
    struct Plane {
        Vec4 localRotation{}, localPosition{};
        Vec4 prevRotation{}, prevPosition{};
        Vec4 rotation{}, position{};
        Vec4 axis{};               // rotate(rotation, +z), cached
        f32 pushScale{};
        i16 anchor{-1};            // raw model bone; -1 = world/model-driven
    };
    struct GroupScratch {
        f32 maxDisplacementSq{};
        bool active{};
    };

    bool PreSolve();               // teleport handling; true when any group went hard
    void AdvanceAnchors(const ClothFrame& frame);      // Step stage 3
    void AdvanceColliders(const ClothAnchor* anim);    // Step stage 4 (local records)
    void StageWorldColliders(const ClothFrame& frame);
    void WindImpulse(f32 dt);
    void Integrate(f32 dt);
    void SolveDistance();
    void SolveBend();
    void SolveTethers();
    void SolveAttachments();
    void CollideCapsules();
    void CollideParticleCapsule(i32 i, const Capsule& c, f32 radiusScale);
    void CollidePlanes();
    void RestPosePull();
    void RecomputeVelocities(f32 dt);
    void BuildOutputFrames();      // triangle-normal sweep + per-particle frames
    // The pinned 4-way skin and the (bind^-1 -> anim) rest skins share this kernel.
    Vec4 SkinPoint(const Particle& p, const Vec4& local, const ClothAnchor* bind,
                   const ClothAnchor* anim, bool translate) const;

    std::vector<Particle> particles_;
    std::vector<Edge> edges_;
    std::vector<Bend> bends_;
    std::vector<Triangle> triangles_;
    std::vector<Capsule> capsules_;
    std::vector<Plane> planes_;
    std::vector<Capsule> worldCapsules_;   // world-collider staging, rebuilt every step
    std::vector<Plane> worldPlanes_;       // world half-space staging
    std::vector<GroupScratch> groups_;
    std::vector<f32> tetherLut_;
    std::vector<i16> anchorMap_;           // raw model bone slot -> anchor state index
    std::vector<ClothAnchor> anchors_;     // this frame's anchor SQTs
    std::vector<ClothAnchor> anchorDeltas_;  // per-step affine deltas between anchor frames
    ClothParams params_{};
    Transform worldPrev_{};
    Transform worldCur_{};
    Vec4 windNoise_{};
    // Frame anchor arrays, raw-slot indexed; valid for the duration of Step (the skinned
    // attachment/rest-pull passes read them mid-solve).
    const ClothAnchor* frameBind_{nullptr};
    const ClothAnchor* frameAnim_{nullptr};
    f32 worldScale_{1.0f};
    f32 maxScaledRestEdge_{0.0f};
    f32 invTetherLutCount_{0.0f};
    i32 pinnedCount_{0};
    bool forceTeleport_{false};
    bool hasSelfCollision_{false};      // any particle flagged (data-level, set at Create)
    bool selfCollisionEnabled_{false};  // the one-way runtime switch
};

}  // namespace snowball
