#pragma once

// ============================================================================
// d3::Emitter — the Diablo III particle simulation: a third dialect beside
// `Emitter2`, sharing the pool, registry, draw list and transparent sort. For an
// ordinary system motion is 100% authored channels, and `eSystemType` picks the
// whole update path; each degradation is named at its site. See
// D3_PARTICLE_DESIGN.md §12 and §30.4.
// ============================================================================

#include "renderer/particle/base/particle_constants.h"
#include "renderer/particle/base/particle_emitter.h"
#include "renderer/particle/base/rnd_seed.h"
#include "renderer/particle/d3/d3_emit_mesh.h"
#include "renderer/particle/d3/d3_emitter_desc.h"
#include "renderer/particle/d3/d3_particle.h"
#include "renderer/particle/d3/d3_path.h"
#include "types.h"
#include "whiteout/flakes/types.h"

#include <functional>
#include <memory>
#include <span>
#include <vector>

namespace whiteout::flakes::renderer::particle::d3 {

/// What one tick of a system reads the same for every particle, built once
/// after the system clock advances — the only place it changes. The two
/// emitter-wide channels are sampled here, once, which is where
/// `ParticleSystem_TickEmitter` samples them too (ch 34 -> sys+0x134
/// @0x71000AEF48, ch 35 -> sys+0x12C @0x71000AEF64).
struct EmitterFrame {
    EvalCtx ctx;              ///< the emitter channels' context
    Vector3f sysPos{0, 0, 0}; ///< the emitter's world position
    f32 unit = 1.0f;          ///< renderer units per `.prt` unit
    f32 invUnit = 1.0f;
    f32 sizeScale = 1.0f;     ///< ch 34, 1 when absent
    f32 effectScale = 1.0f;   ///< ch 35
    bool hasEffectScale = false;
};

class Emitter final : public particle::ParticleEmitter {
public:
    Emitter();

    Emitter* AsD3() override {
        return this;
    }
    const Emitter* AsD3() const override {
        return this;
    }

    /// Never null: an emitter without one reads a static default whose channels
    /// are all empty, which emits nothing rather than crashing.
    void SetD3Desc(std::shared_ptr<const EmitterDesc> desc);
    const EmitterDesc& D3Desc() const {
        return *d3desc_;
    }

    void Update(f32 elapsed, f32 emissionScaler) override;
    i32 BuildGeometry(const BuildGeometryInput& in, std::vector<Vertex>& out) const override;
    void CollectOutputEvents(std::vector<ChildModelEvent>& out) override;

    /// Seeds the system stream and the emitter-channel seed (`sys+28`) from
    /// @p seed, so two actors of one `.prt` do not emit one cloud. An emitter
    /// never seeded keeps the fixed pair below and is still reproducible.
    void SetSystemSeed(u32 seed) {
        sysRng_ = MwcRng::Seed(MixSeed(seed));
        emitterSeed_ = MixSeed(seed ^ kD3ChannelSeedSalt);
    }

    /// The desc's material and priority, in the id space its output declares:
    /// a type 1/3/4 system draws nothing of its own — every emission is a whole
    /// model — so it registers as a child-model emitter and the geometry
    /// builder is never asked for it.
    EmitterDrawHeader DrawHeader() const override;

    i32 TotalAlive() const override {
        return static_cast<i32>(pool_.AliveCount());
    }

    /// @brief Who the children belong to, and what mints their handles.
    ///
    /// Same seam ChildModelEmitter takes, and for the same reason: the emitter
    /// reports births as data and the actor layer owns the spawn. Without an
    /// allocator such a system counts its emissions and produces nothing, which
    /// is the right answer for a test that only wants the simulation.
    void SetChildOwner(u32 owner, i32 emitterId, std::function<u32()> alloc) {
        children_.Bind(owner, emitterId, std::move(alloc));
    }

    /// How many child actors this system has spawned. The engine's emit clamp
    /// counts these and not particles (`sys+408 + sys+376`), which is what makes
    /// a target count of 1 — 4,189 of 4,795 shipped files — mean "one model".
    i32 ChildCount() const {
        return static_cast<i32>(children_.LiveCount());
    }

    /// The emitter's orientation: rotates the birth velocity and the
    /// emitter-local kinematic triple (§5.1). Identity makes triple B
    /// world-space, as the engine does for an unrotated emitter. See §30.4.
    void SetEmitterOrientation(const Quaternion& q) {
        emitterQuat_ = q;
    }

    /// The bound spawn target the seek model converges on. Without one the
    /// model is inert, which is what the engine does when nothing is bound. No
    /// host binds one yet, so the seek arm is unreachable outside a direct
    /// call.
    void SetSeekTarget(const Vector3f& t) {
        seekTarget_ = t;
        hasSeekTarget_ = true;
    }
    void ClearSeekTarget() {
        hasSeekTarget_ = false;
    }

    /// @brief The camera's view direction (`view+0x28C`), for the modes that
    ///        face it — at draw, and at emit for a child-actor system whose
    ///        render mode 0 or 13 turns the spawned MODEL (§30.4).
    void SetCameraForward(const Vector3f& f) {
        camForward_ = f;
    }

    /// Global wind, for the two foliage system types. `phase` is a shared
    /// clock; each particle offsets it by its own random so a gust travels. No
    /// host sets it yet: the spring runs at strength 0 and only settles.
    void SetWind(const Vector2f& dir, f32 strength, f32 phase) {
        windDir_ = dir;
        windStrength_ = strength;
        windPhase_ = phase;
    }

    /// Which of the model's bones this emitter rides (-1, `Default`, is the
    /// model origin) and the hardpoint's local frame composed onto it; there
    /// are no per-frame emitter tracks. See io/d3/d3_effect_resolver.h, §30.4.
    void SetAttachBone(i32 bone) {
        attachBone_ = bone;
    }
    i32 AttachBone() const {
        return attachBone_;
    }
    void SetAttachOffset(const Matrix44f& m) {
        attachOffset_ = m;
    }
    const Matrix44f& AttachOffset() const {
        return attachOffset_;
    }

    /// Seconds since the system was spawned, and whether `tmLifetime` has run
    /// out. A finished system stops emitting but keeps its live particles.
    f32 SystemAge() const {
        return systemAge_;
    }
    bool EmissionFinished() const;

    /// Per-particle state, index-parallel with the pool. Exposed for the trace
    /// harness and the tests; nothing in the render path needs it.
    const std::vector<ParticleState>& States() const {
        return states_;
    }

    /// How many particles were emitted during the last Update. The emission
    /// gate asserts on this rather than on a pool count, because a particle
    /// born and killed inside one step would otherwise be invisible.
    i32 EmittedLastUpdate() const {
        return emittedLastUpdate_;
    }

    /// Restart the system: age, accumulator, pool and the placement the next
    /// move measures from. Used when an actor re-triggers an effect rather than
    /// respawning it.
    void Restart();

    /// A scene rewind is a re-trigger, so it is the same call.
    void ResetParticles() override {
        Restart();
    }

private:
    /// Grow the pool, and the per-particle state index-parallel with it.
    void GrowPool(u32 capacity);

    /// What `ParticleSystem_TickEmitter` precomputes once per tick and
    /// `ParticleSystem_SampleEmitterShape` reads per particle.
    struct EmitContext {
        Shape shape = Shape::Point;
        /// Never written, so zero. Kept as a term of the shape sum because
        /// dropping its `0 +` can flip the sign of a zero position.
        Vector3f offset{0, 0, 0};
        /// The two LANES of each shape-extent path at the emitter's current
        /// time, as `InterpolationPath_GetScalarEndpoints` hands them over —
        /// not a min and a max over the whole curve.
        f32 ext0Lo = 0.0f, ext0Hi = 0.0f;
        f32 ext1Lo = 0.0f, ext1Hi = 0.0f;
        Vector3f boxLo{0, 0, 0};
        Vector3f boxHi{0, 0, 0};
        i32 emitCount = 0;
        i32 emitIndex = 0;
        f32 ringPhase = 0.0f;
    };

    /// One area-weighted (or, for shape 11, sequential) point on
    /// `surface_.mesh`, skinned and taken to renderer units. False leaves the caller
    /// on the point case.
    bool SampleEmitMeshPoint(bool sequential, u32 sequence, Vector3f& out);
    Vector3f SkinEmitMeshVertex(const EmitMesh& m, u32 vertex) const;

    EmitterFrame BuildEmitterFrame() const;

    EvalCtx EmitterCtx() const;
    EvalCtx ParticleCtx(const ParticleState& st, const Vector3f& pos, f32 age,
                        const EmitterFrame& f) const;
    /// The material's atlas layer, per particle. See the definitions.
    void SeedUvStates(ParticleState& st) const;
    void StepUvStates(ParticleState& st, f32 dt) const;

    void TickEmit(f32 dt, f32 emissionScaler, const EmitterFrame& f);
    EmitContext BuildEmitContext(const EvalCtx& ectx) const;
    Vector3f SampleShape(EmitContext& ec, const Vector3f& base);
    bool BirthParticle(f32 dt, EmitContext& ec, const EmitterFrame& f);
    /// `Particle_ComputeInitialVelocity` — channels 38/39/40, in `.prt` units
    /// per second. Draws from `sysRng_` when the cone applies, so it is taken
    /// only where the engine takes it: at the end of a type 2, 3 or 9 birth
    /// record, and again when a child actor spawns.
    Vector3f BirthVelocity(u32 seed, const EvalCtx& ectx);
    bool SpawnChildActor(f32 dt, EmitContext& ec, const EmitterFrame& f);
    /// One `ParticleSystem_UpdateParticles`. False means the particle died in it.
    bool StepParticle(u32 idx, f32 dt, const EmitterFrame& f);
    /// @p pos stood on the ground under its XY, @p height above it. False over
    /// no ground; true and untouched when no host installed a query.
    bool SnapToGround(Vector3f& pos, f32 height) const;
    /// `dwPrtFlags` bit 2's birth placement. False drops the birth.
    bool PlaceOnGround(Vector3f& pos) const;
    void StepWindSpring(f32 dt);
    /// `ParticleSystem_SetEmitterTransform` @0x71000AFBE0's carry, G-D3P-21.
    void CarryWithEmitter();

    /// `Particle.tLifetimeRandom` resolved once per spawn — see
    /// @ref EmitterDesc::lifetimeRandom. Never touches `sysRng_`: the engine's
    /// draw comes off a global stream, and taking one out of the system stream
    /// would shift every positional draw after it.
    f32 LifetimeScale() const;

    /// `Particle.tmPreSimulate` — the spawn-time catch-up, run once on the
    /// first Update. See the definition.
    void RunPreSimulate();

    /// `Particle_UpdateGroundNormal` @0x71000BA330's job, against the shared
    /// ground query: the normal from two height tangents (world up on the grid,
    /// the engine's raycast-miss value), re-sampled only when XY moved (§30.4).
    void RefreshGroundNormal(ParticleState& st, const Vector3f& pos) const;

    std::shared_ptr<const EmitterDesc> d3desc_;
    std::vector<ParticleState> states_;

    /// The system's own random stream — particle seeds and the shape draws
    /// come out of it. The WC3/WoW `CRandom` stream is `Emitter2`'s and this
    /// class has none.
    MwcRng sysRng_{0x1234567u, 666u};
    /// The seed emitter-side channel evaluations use (`sys+28` in the engine).
    u32 emitterSeed_ = 1;

    f32 systemAge_ = 0.0f;
    f32 emitAccum_ = 0.0f;
    f32 lifetimeScale_ = 1.0f;
    bool preSimPending_ = false;
    i32 emittedLastUpdate_ = 0;
    u32 emitSequence_ = 0; ///< shape 11's monotone counter; never reset


    i32 attachBone_ = -1;
    Matrix44f attachOffset_ = Matrix44f::identity();
    Quaternion emitterQuat_ = Quaternion::identity();

    // The emitter placement the live particles were last carried to. Separate
    // from `placement_.prevWorldPos`, which is the birth-lerp segment and
    // moves on a different schedule.
    Vector3f carryPos_{0, 0, 0};
    Quaternion carryQuat_ = Quaternion::identity();
    bool carrySeeded_ = false;
    Vector3f seekTarget_{0, 0, 0};
    bool hasSeekTarget_ = false;

    Vector3f camForward_{0, 1, 0};

    Vector2f windDir_{1, 0};
    f32 windStrength_ = 0.0f;
    f32 windPhase_ = 0.0f;

    // The child actors a type 1/3/4 system has spawned, slotted by birth
    // order, and the events not yet drained. Not pool indices: these are not
    // particles, and the engine keeps them in a list of its own for exactly
    // the same reason.
    ChildOutputChannel children_;
};

/// The five random draws the shape sampler makes, exposed so the shape gate
/// can assert on the distribution without standing up a whole emitter.
/// @{
Vector3f SamplePointOnSphere(MwcRng& rng, f32 radius);
Vector3f SamplePointOnHemisphere(MwcRng& rng, f32 radius);
Vector3f SamplePointOnCircleXY(MwcRng& rng, f32 radius);
f32 SampleRadiusInAnnulus(MwcRng& rng, f32 inner, f32 thickness);
/// @}

/// @brief What `Particle_InitLifeAndSize` @0x71000B6BB0 writes after the shape
///        sample, in its order, into @p st (whose seed must already be set):
///        base size (ch 28), lifetime (ch 29, else ONE frame) shortened by ch 30,
///        then up to three draws — spin axis, orbit phase, roll. G-D3P-A4
///        replays it. See §4.5 and §30.4.
void InitLifeAndSize(MwcRng& rng, const EmitterDesc& d, const EvalCtx& ctx, const Vector3f& prev,
                     const Vector3f& now, f32 invUnit, f32 dt, const Quaternion& emitterQuat,
                     ParticleState& st);

} // namespace whiteout::flakes::renderer::particle::d3
