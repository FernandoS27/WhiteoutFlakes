#pragma once

#include "core/particle_dialect.h"
#include "emitter_desc.h"
#include "particle2.h"
#include "particle_emitter.h"
#include "particle_output.h"
#include "particle_shape.h"
#include "rnd_seed.h"
#include "sc2_kernel_types.h"
#include "trail_set.h"
#include "types.h"
#include "wow_runtime.h"
// FrameState::ParticleFrameState is a nested type, so it cannot be forward
// declared — ApplyState takes it by reference and needs the full definition.
#include "whiteout/flakes/model_types.h"
#include "whiteout/flakes/types.h"

#include <memory>
#include <span>
#include <vector>

namespace whiteout::flakes::renderer::particle {

using ParticleBehavior = core::ParticleBehavior;

class ChildModelEmitter;
struct Sc2Runtime;

/// What one run of a WC3-family emitter accumulates. A rewind replaces it
/// whole, so a member added here cannot silently survive one.
struct Wc3RunState {
    /// The fractional particle the rate has not released yet.
    f32 numNew = 0.0f;
    /// A one-shot burst is still owed.
    bool squirtOwed = false;
};

// One concrete emitter. What used to be expressed by subclassing (PlaneEmitter)
// is now composition: the shared desc carries the spawn shape, so a sphere or
// cone emitter is a different desc rather than a different class.
class Emitter2 : public ParticleEmitter {
public:
    Emitter2();
    /// Takes this emitter's entries out of the pending list it is attached to,
    /// so the service's walk never reaches a runtime that is gone.
    ~Emitter2() override;

    Emitter2* AsEmitter2() override {
        return this;
    }
    const Emitter2* AsEmitter2() const override {
        return this;
    }
    /// The child-model outputs answer themselves; see ChildModelEmitter.
    virtual ChildModelEmitter* AsChildModel() {
        return nullptr;
    }

    /// Whether this emitter runs the StarCraft II simulation — what the actor
    /// layer asks instead of reading the desc's family.
    bool IsSc2() const {
        return sc2_ != nullptr;
    }

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

    void SetSquirtPending(bool v) {
        run_.squirtOwed = v;
    }
    /// Whether a one-shot burst is still owed. Readable because SetDesc arms it
    /// unconditionally for a squirting emitter, so anything that re-describes a
    /// LIVE emitter has to put the real answer back or make it burst twice.
    bool SquirtPending() const {
        return run_.squirtOwed;
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

    // ---- the dialect-neutral SC2 surface ----
    //
    // All of these are no-ops while `sc2_` is null, which is every emitter of
    // every other family. They are declared on the base rather than on an SC2
    // subclass because their callers — the service, the actor layer, the
    // loader — must not know which family they are holding, which is the same
    // reason SetDesc and ApplyState are here (design §3.4).

    /// A request another emitter made of this one; queued for the next EMIT.
    /// Silently dropped past retail's 128-entry cap rather than growing.
    void QueueSpawnRequest(const SpawnRequest& req);

    /// Move this emitter's outgoing requests to @p out, leaving it empty. The
    /// service drains after Update and delivers each to its target's inbox.
    void DrainSpawnRequests(std::vector<RoutedSpawnRequest>& out);

    /// Owe @p slot a burst of @p count particles — the actor layer's half of
    /// the crossed squirt keys.
    void QueueBurst(u32 slot, u32 count);

    /// The model's active sequence this frame (`Sc2ActiveSequence`). A change
    /// asks a `SimulateInit` emitter for a pre-roll on the next tick; whether
    /// one runs is gap-driven and PREP's decision, not the caller's (RE §15.4).
    void SetSc2ActiveSequence(i32 sequence);

    /// Where this emitter registers the ModelParticles elements waiting for a
    /// model. The service installs its frame-wide list at registration; until
    /// then the emitter keeps its own and walks it at the end of each tick.
    void AttachSc2PendingList(Sc2PendingModels* list);

    /// One entry of the pending walk (`ProcessPendingSpawns`, OP14b): draw the
    /// element's model path and random direction, pose it, report the birth.
    /// An element already dead, or an emitter with no paths, draws nothing.
    void ServiceSc2PendingModel(i32 node);

    /// What an SC2 model particle's pose reads from the scene each frame: the
    /// camera, and the owning actor's world scale.
    void SetSc2Scene(const Matrix44f& worldToView, f32 actorWorldScale);

    // Deterministic per-emitter RNG seed. Callers derive it from stable identity
    // (actor handle + emitter index) so the same scene reproduces run to run.
    void SetSeed(u32 seed);

    void Squirt() {
        run_.squirtOwed = true;
    }

    /// See @ref WowRuntime::lifeSpan.
    void SetLifeSpan(f32 s) {
        wow_.lifeSpan = s;
    }

    /// See @ref WowRuntime::viewDistance. Ignored under the WC3 dialect.
    void SetViewDistance(f32 d) {
        wow_.viewDistance = d;
    }

    /// See @ref WowRuntime::boneSpawns.
    void SetBoneSpawnTable(
        std::span<const model::FrameState::BoneSpawn> table) {
        wow_.boneSpawns.assign(table.begin(), table.end());
        spawn_.boneTable = wow_.boneSpawns;
    }

    /// See @ref WowRuntime::modelAlpha.
    f32 ModelAlpha() const {
        return wow_.modelAlpha;
    }

    // This emitter's random flipbook offset, added to every cell it draws. Drawn
    // once from the emitter's seed, so two actors of the same model animate
    // their sheets out of phase.
    u16 BaseCell() const {
        return wow_.baseCell;
    }

    // What one particle's lifespan actually is: uniform under WC3, per-particle
    // and re-evaluated against the current animated track under WoW — so a
    // lifespan keyframe retro-actively resizes particles already in flight.
    f32 EffectiveLifeSpan(const Particle2& p) const;

    void Update(f32 elapsed, f32 emissionScaler) override;

    /// The WC3/WoW arm is the free function `BuildEmitterGeometry`, unchanged;
    /// the SC2 arm is @ref BuildSc2Geometry.
    i32 BuildGeometry(const BuildGeometryInput& in, std::vector<Vertex>& out) const override;

    /// The SC2 arm of @ref BuildGeometry, out of line because it reaches into
    /// `sc2_` and the WC3 builder must not.
    static i32 BuildSc2Geometry(const Emitter2& e, const BuildGeometryInput& in,
                                std::vector<Vertex>& out);

    /// The SC2 arm of `InternalUpdate`. Assembles the frame `Sc2TickEmitter`
    /// takes and runs it; the composition itself lives in `sc2_tick.cpp` so it
    /// can be driven without an emitter.
    void TickSc2(f32 elapsed, f32 emissionScaler);

    void ResetParticles() override;

    // Adopt one trail emitter: from here on, every live particle of this
    // emitter drives its emission once per sub-step. See TrailSet::Adopt.
    void AddTrail(std::unique_ptr<Emitter2> trail);

    /// The record state a trail runs on; see @ref TrailState.
    void SetTrailState(const model::FrameState::ParticleFrameState& st) {
        trailState_ = TrailState::From(st);
    }

    /// The adopted trails, in adoption order; see @ref TrailSet::All.
    const std::vector<std::unique_ptr<Emitter2>>& Trails() const {
        return trails_.All();
    }
    static constexpr usize kMaxTrails = TrailSet::kMax;

    f32 EmissionRate() const {
        return emissionRate_;
    }

    // The extra texture layers, index-parallel with the pool. Empty unless the
    // desc asked for refraction or multi-texture — see MultiTexState.
    const std::vector<MultiTexState>& MultiTex() const {
        return multiTex_;
    }

    /// An SC2 emitter's live particles are in its runtime's store, everyone
    /// else's in the pool. Out of line so the runtime stays a forward
    /// declaration here.
    i32 TotalAlive() const override;

    EmitterDrawHeader DrawHeader() const override;

protected:
    // Grow the pool and tell the output about it. Split out of Sync so a parent
    // can size its trails' pools the way the client does.
    void GrowPool(u32 capacity);

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

    /// The stream every birth draws from. An output that draws more at birth —
    /// an M2 model particle's tumble — takes those draws from here, straight
    /// after the base's, which is the client's order and what the trace
    /// compares.
    RndSeed& SpawnStream() {
        return randSeed_;
    }

    /// The SC2 runtime, or null for every other family. Read-only for an
    /// output: the runtime's writers are this class's SC2 touch points.
    const Sc2Runtime* Sc2State() const {
        return sc2_.get();
    }

private:
    // Spawn one particle: age jitter, then the shape's position/velocity draw,
    // then the emitter-space transform.
    void CreateParticle(Particle2& p, f32 elapsed);

    /// One birth, in the client's order: a free slot, the spawn draws, the
    /// extra texture layers' draws, then the hook.
    void BirthOne(f32 elapsed);
    /// One death at alive-list position @p alivePos (pool index @p idx):
    /// the hook first, while the particle is still readable.
    void KillAt(usize alivePos, u32 idx);

    /// Everything @ref ApplyState writes except the SC2 sample block, from any
    /// state carrying the same field names — the model's own tracks, or a
    /// trail's @ref TrailState.
    template <class State>
    void ApplyAnimated(const State& st);

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

    /// The SC2 halves of SetDesc, ResetParticles and ApplyState, in
    /// particle2_emitter_sc2.cpp. Each expects `sc2_` to exist.
    void DescribeSc2();
    void RewindSc2();
    /// The runtime's emitter-region triangle table over the surface's mesh,
    /// rebuilt when the mesh or the desc behind it changed.
    void RefreshSc2MeshTriangles();
    void ApplySc2Frame(const model::FrameState::ParticleFrameState::Sc2ParticleFrame& frame);

    std::shared_ptr<const EmitterDesc> desc_;
    ParticleBehavior behavior_ = ParticleBehavior::Wc3();

    // The SC2 runtime, allocated by SetDesc when the desc names that family
    // and freed otherwise. NULL is the family test every touch point below
    // uses, so `EmitterDesc::family` is read in exactly one place and every
    // other dialect pays one pointer for the whole dialect (design R1, §2.1).
    std::unique_ptr<Sc2Runtime> sc2_;
    /// The service's pending list, kept here as well as in the runtime so an
    /// emitter attached before its desc names the SC2 family still gets it.
    Sc2PendingModels* sc2PendingList_ = nullptr;

    /// Reset whole by a rewind.
    Wc3RunState run_;
    /// Adopted as another emitter's trail (M2 RPID), for the life of the
    /// adoption. Two consequences, both of them the client's: such an emitter
    /// never emits from its own position (`StepUpdate` @0x1016a95c0 recurses
    /// with suppressEmit set, and that is the only way a trail is ever
    /// updated), and its enable is sticky rather than re-asserted per frame
    /// (`RecursiveEmitterModelLoaded` @0x1016a6b60 ORs it once at adoption,
    /// because nothing animates a trail afterwards).
    bool isTrail_ = false;

    SpawnParams spawn_;
    MotionParams motion_;
    f32 emissionRate_ = 0.0f;

    WowRuntime wow_;

    // Renderer units -> the space this emitter's particles are stored in.
    //
    // Every emitter-motion quantity (travel, inherited velocity, follow delta)
    // is measured on the world transform, so it arrives in renderer units,
    // while a model-space particle keeps its position in model units. The
    // client mixes the two freely because its world IS model units; here they
    // differ by WorldScale, and adding one to the other displaces a particle a
    // hundred times further than the record asked for.
    f32 MotionToParticleSpace() const {
        return (desc_->modelSpace && placement_.unitScale > 0.0f) ? (1.0f / placement_.unitScale)
                                                                  : 1.0f;
    }

    RndSeed randSeed_;
    RndSeed compactSeed_;

    // Index-parallel with the pool, and sized only for an emitter that asked
    // for the extra layers (refraction or multi-texture).
    std::vector<MultiTexState> multiTex_;

    TrailSet trails_;
    // Only meaningful on a trail (`isTrail_`); see SetTrailState.
    TrailState trailState_;

    friend class TrailSet;
};

} // namespace whiteout::flakes::renderer::particle
