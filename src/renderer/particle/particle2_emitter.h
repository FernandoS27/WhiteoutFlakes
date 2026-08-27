#pragma once

#include "core/particle_dialect.h"
#include "emitter_desc.h"
#include "particle2.h"
#include "particle_pool.h"
#include "particle_shape.h"
#include "rnd_seed.h"
#include "types.h"
// FrameState::ParticleFrameState is a nested type, so it cannot be forward
// declared — ApplyState takes it by reference and needs the full definition.
#include "whiteout/flakes/model_types.h"
#include "whiteout/flakes/types.h"

#include <memory>
#include <span>
#include <vector>

namespace whiteout::flakes::renderer::particle {

using ParticleBehavior = core::ParticleBehavior;

// Only the two genuinely per-frame flags survive: everything else that used to
// live here (head/tail, sortZ, model space, XY quads) is fixed at load time and
// now reads off the shared EmitterDesc.
enum EmitterFlag : u32 {
    kFlagVisible = 0x001,
    kFlagNeedSquirt = 0x020,
    // Adopted as another emitter's trail (M2 RPID). Two consequences, both of
    // them the client's: such an emitter never emits from its own position
    // (`StepUpdate` @0x1016a95c0 recurses with suppressEmit set, and that is the
    // only way a trail is ever updated), and its enable bit is sticky rather
    // than re-asserted per frame (`RecursiveEmitterModelLoaded` @0x1016a6b60 ORs
    // it once at adoption, because nothing animates a trail afterwards).
    kFlagTrail = 0x040,
};

// One concrete emitter. What used to be expressed by subclassing (PlaneEmitter)
// is now composition: the shared desc carries the spawn shape, so a sphere or
// cone emitter is a different desc rather than a different class.
class Emitter2 {
public:
    Emitter2();
    virtual ~Emitter2() = default;

    // Per-frame animated state from the model's FrameState. Virtual so an
    // output kind that reads different fields (child models use latitude and
    // longitude but no plane extent) can override without the service or
    // actor-eval knowing which kind it is holding.
    virtual void ApplyState(const model::FrameState::ParticleFrameState& st);

    // The immutable half, shared across every actor spawned from one model.
    // Never null: an emitter without one reads a static default.
    void SetDesc(std::shared_ptr<const EmitterDesc> desc);
    const EmitterDesc& Desc() const {
        return *desc_;
    }

    // Which client's simulation this emitter runs. Set once at registration
    // from the profile — a load-time decision, like the ribbon dialect, so
    // callers read LoadTimeProfile() rather than the frame latch.
    void SetBehavior(const ParticleBehavior& b) {
        behavior_ = b;
    }
    const ParticleBehavior& Behavior() const {
        return behavior_;
    }

    void SetVisible(bool v) {
        SetFlag(kFlagVisible, v);
    }
    void SetSquirtPending(bool v) {
        SetFlag(kFlagNeedSquirt, v);
    }
    /// Whether a one-shot burst is still owed. Readable because SetDesc arms it
    /// unconditionally for a squirting emitter, so anything that re-describes a
    /// LIVE emitter has to put the real answer back or make it burst twice.
    bool SquirtPending() const {
        return (flags_ & kFlagNeedSquirt) != 0;
    }

    // ---- animated per frame from FrameState ----
    void SetEmissionRate(f32 v) {
        emissionRate_ = v;
    }
    MotionParams& Motion() {
        return motion_;
    }
    const MotionParams& Motion() const {
        return motion_;
    }
    SpawnParams& Spawn() {
        return spawn_;
    }
    const SpawnParams& Spawn() const {
        return spawn_;
    }

    // Outputs that drive something outside the vertex stream (child actors
    // today) report what happened during the last Update here. Default no-op.
    virtual void CollectOutputEvents(std::vector<struct ChildModelEvent>& out) {}

    // Deterministic per-emitter RNG seed. Callers derive it from stable identity
    // (actor handle + emitter index) so the same scene reproduces run to run.
    void SetSeed(u32 seed);

    void Squirt() {
        flags_ |= kFlagNeedSquirt;
    }

    void SetModelToWorld(const Matrix44f& m) {
        modelToWorld_ = m;
    }

    // The animated lifespan. WoW drives this from an M2 track and re-reads it
    // every frame for every live particle; WC3 has no such track, so its
    // emitters leave it at the desc value and nothing consults it.
    void SetLifeSpan(f32 s) {
        lifeSpan_ = s;
    }

    // Distance from the camera, for WoW's emission-rate falloff. Pushed by the
    // service once per frame; ignored entirely under the WC3 dialect.
    void SetViewDistance(f32 d) {
        viewDistance_ = d;
    }

    // The model's bone-emitter table, for a bone generator. Copied rather than
    // referenced: the FrameState it comes from is rebuilt every evaluation and
    // the emitter reads this during its own update, which happens later.
    void SetBoneSpawnTable(
        std::span<const model::FrameState::BoneSpawn> table) {
        boneSpawns_.assign(table.begin(), table.end());
        spawn_.boneTable = boneSpawns_;
    }

    // Renderer units per model unit. The `.m2` size, twinkle and tail values are
    // authored in yards while the emitter draws in renderer units, so the
    // geometry builder converts with this. MDX models are already in renderer
    // units and leave it at 1.
    f32 UnitScale() const {
        return unitScale_;
    }

    // The model's own fade, which WoW multiplies into particle alpha instead of
    // gating the emitter with it.
    f32 ModelAlpha() const {
        return modelAlpha_;
    }

    // This emitter's random flipbook offset, added to every cell it draws. Drawn
    // once from the emitter's seed, so two actors of the same model animate
    // their sheets out of phase.
    u16 BaseCell() const {
        return baseCell_;
    }

    // Emitter world position for this frame. WoW spawns along the segment
    // between the previous frame's position and this one, and derives the
    // velocity a particle inherits from the same delta.
    void SetWorldPosition(const Vector3f& p);

    // What one particle's lifespan actually is: uniform under WC3, per-particle
    // and re-evaluated against the current animated track under WoW — so a
    // lifespan keyframe retro-actively resizes particles already in flight.
    f32 EffectiveLifeSpan(const Particle2& p) const;

    // `emissionScaler` multiplies the emission rate. Threaded in from the owning
    // service rather than read from a global, so two scenes can scale
    // independently.
    void Update(f32 elapsed, f32 emissionScaler);

    // Adopt one trail emitter: from here on, every live particle of this
    // emitter drives its emission once per sub-step. Capped at four, the
    // client's MAX_CHILD_EMITTERS (ParticleSystem2.cpp:2503) — the fifth is
    // dropped, as the client's assert intends.
    void AddTrail(std::unique_ptr<Emitter2> trail);

    // The animated state a trail runs on. A trail has no tracks of its own —
    // the model its record came from is never placed, so nothing walks them —
    // so its owner re-applies this every frame, patching in the two fields that
    // belong to the OWNING actor rather than to the record: the unit scale
    // (which decides how the gravity vector is converted) and the model alpha.
    void SetTrailState(const model::FrameState::ParticleFrameState& st) {
        trailState_ = st;
    }

    // The adopted trails, in adoption order. The service walks these to build
    // their geometry and to count them; nothing else may drive them.
    const std::vector<std::unique_ptr<Emitter2>>& Trails() const {
        return trails_;
    }
    static constexpr usize kMaxTrails = 4;

    bool Visible() const {
        return (flags_ & kFlagVisible) != 0;
    }
    u32 Flags() const {
        return flags_;
    }

    // ---- forwarded from the shared desc ----
    bool HasHead() const {
        return desc_->hasHead;
    }
    bool HasTail() const {
        return desc_->hasTail;
    }
    bool SortZ() const {
        return desc_->sortZ;
    }
    bool UseModelSpace() const {
        return desc_->modelSpace;
    }
    bool XYQuads() const {
        return desc_->xyQuads;
    }
    CoordSpace GetCoordSpace() const {
        return desc_->coordSpace;
    }
    f32 LifeSpan() const {
        return desc_->lifeSpan;
    }
    f32 AngularVelocity() const {
        return desc_->angularVelocity;
    }
    f32 TailLength() const {
        return desc_->tailLength;
    }
    ParticleOutput Output() const {
        return desc_->output;
    }
    i32 PriorityPlane() const {
        return desc_->priorityPlane;
    }
    const SpriteSheet& Sheet() const {
        return desc_->sheet;
    }
    const LifetimeCurves& Curves() const {
        return desc_->curves;
    }
    const ParticleMaterialDesc& Material() const {
        return desc_->material;
    }

    f32 EmissionRate() const {
        return emissionRate_;
    }
    const Matrix44f& ModelToWorld() const {
        return modelToWorld_;
    }

    // This frame's emitter position, after SetWorldPosition. Read by the sim
    // tests to bound where a spawn may land relative to the emitter.
    const Vector3f& WorldPosition() const {
        return worldPos_;
    }

    const ParticlePool& Pool() const {
        return pool_;
    }
    ParticlePool& Pool() {
        return pool_;
    }

    // The extra texture layers, index-parallel with the pool. Empty unless the
    // desc asked for refraction or multi-texture — see MultiTexState.
    const std::vector<MultiTexState>& MultiTex() const {
        return multiTex_;
    }

    i32 TotalAlive() const {
        return static_cast<i32>(pool_.AliveCount());
    }

protected:
    // Spawn one particle: age jitter, then the shape's position/velocity draw,
    // then the emitter-space transform. Virtual only so a child-model output can
    // hook birth; the shape choice is data, not a subclass.
    virtual void CreateParticle(Particle2& p, f32 elapsed);

    void SetFlag(u32 mask, bool on) {
        if (on)
            flags_ |= mask;
        else
            flags_ &= ~mask;
    }

    // Whether a spawn is displaced along the emitter's path this frame. False
    // for a random-spacing emitter because the client discards the point it
    // drew for one (see EmitStep), and false in model space because the client
    // reads the spawn matrix only on the branch that bakes to world — so for a
    // world-space emitter the two are algebraically identical and for a local
    // one the path does not exist at all.
    bool PathOffsetsSpawn() const {
        return behavior_.emitAlongPath && !desc_->modelSpace && !desc_->randomEmissionSpacing;
    }

    void InternalUpdate(f32 elapsed, f32 emissionScaler);

    // One simulation step: release this step's particles, then age, kill and
    // move the live ones. WC3 runs it once per frame; WoW runs it once per
    // fixed sub-step plus once for the remainder.
    void StepOnce(f32 dt, f32 emissionScaler);
    void EmitStep(f32 dt, f32 emissionScaler);
    void AdvanceStep(f32 dt, f32 emissionScaler);

    // One live particle drives every trail's emission, at its own position.
    void DriveTrails(const Particle2& p, f32 dt, f32 emissionScaler);

    // Grow the pool and tell the output about it. Split out of Sync so a parent
    // can size its trails' pools the way the client does.
    void GrowPool(u32 capacity);

    // Draw one newly born particle's extra texture layers. No-op unless the
    // desc asked for the layers; when it did, this is six draws off the
    // emitter's own stream, immediately after CreateParticle, exactly where
    // `CreateParticle(CMultiTexParticle&)` @0x1016a10b0 takes them.
    void SeedMultiTex(u32 poolIndex);

    // Advance one live particle's layers by `dt` and wrap each back into
    // [0,1). Runs before the move, where `UpdateLiveParticle<CMultiTexParticle>`
    // @0x1016a9d30 puts it.
    void AdvanceMultiTex(u32 poolIndex, f32 dt);

    // Emitter velocity refresh — WoW only, and only while the pool is empty.
    void TickEmitterVelocity(f32 dt);

    void Sync();

    // Called after the pool grows, with the new particle capacity. Outputs that
    // keep per-particle side data (a child actor handle, a rotation, a spawn
    // seed) resize it here so it stays index-parallel with the pool. Particle2
    // is frozen at 32 bytes to mirror the engine layout, so extra per-particle
    // state has to live alongside rather than inside it.
    virtual void OnPoolResized(usize capacity) {}

    // Fired once per particle birth and once per death, keyed by pool index.
    // Per-birth and per-death only — never per particle per frame, so the
    // integration loop stays non-virtual. This is what lets a particle own an
    // external resource (a child actor now; a light, decal or sub-emitter
    // later) with a defined release point.
    virtual void OnParticleBorn(u32 poolIndex) {}
    virtual void OnParticleDied(u32 poolIndex) {}

protected:
    std::shared_ptr<const EmitterDesc> desc_;
    ParticleBehavior behavior_ = ParticleBehavior::Wc3();

    u32 flags_ = 0;

    SpawnParams spawn_;
    MotionParams motion_;
    f32 emissionRate_ = 0.0f;

    f32 numNew_ = 0.0f;
    f32 lifeSpan_ = 0.0f;

    Matrix44f modelToWorld_ = Matrix44f::identity();

    // Renderer units -> the space this emitter's particles are stored in.
    //
    // Every emitter-motion quantity (travel, inherited velocity, follow delta)
    // is measured on the world transform, so it arrives in renderer units,
    // while a model-space particle keeps its position in model units. The
    // client mixes the two freely because its world IS model units; here they
    // differ by WorldScale, and adding one to the other displaces a particle a
    // hundred times further than the record asked for.
    f32 MotionToParticleSpace() const {
        return (desc_->modelSpace && unitScale_ > 0.0f) ? (1.0f / unitScale_) : 1.0f;
    }

    // ---- WoW emitter-motion state (inert under the WC3 dialect) ----
    Vector3f worldPos_{0, 0, 0};
    Vector3f prevWorldPos_{0, 0, 0};
    bool worldPosSeeded_ = false;
    Vector3f emitterVelocity_{0, 0, 0};
    Vector3f spawnOffset_{0, 0, 0};
    Vector3f followDelta_{0, 0, 0};
    f32 velocityTimer_ = 0.0f;
    f32 viewDistance_ = 0.0f;
    f32 unitScale_ = 1.0f;
    f32 modelAlpha_ = 1.0f;
    std::vector<model::FrameState::BoneSpawn> boneSpawns_;
    u16 baseCell_ = 0;

    RndSeed randSeed_;
    RndSeed compactSeed_;

    ParticlePool pool_;

    // Index-parallel with the pool, and sized only for an emitter that asked
    // for the extra layers (refraction or multi-texture).
    std::vector<MultiTexState> multiTex_;

    // Owned outright, like the client's recursion model owns the emitters the
    // parent borrows pointers to. Never re-entrant: a trail's own trails are
    // dropped at load, so this vector is at most one level deep.
    std::vector<std::unique_ptr<Emitter2>> trails_;

    // Only meaningful on an emitter carrying kFlagTrail; see SetTrailState.
    model::FrameState::ParticleFrameState trailState_{};
};

} // namespace whiteout::flakes::renderer::particle
