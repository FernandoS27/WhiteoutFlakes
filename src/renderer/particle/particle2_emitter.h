#pragma once

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
#include <vector>

namespace whiteout::flakes::renderer::particle {

// Only the two genuinely per-frame flags survive: everything else that used to
// live here (head/tail, sortZ, model space, XY quads) is fixed at load time and
// now reads off the shared EmitterDesc.
enum EmitterFlag : u32 {
    kFlagVisible = 0x001,
    kFlagNeedSquirt = 0x020,
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

    void SetVisible(bool v) {
        SetFlag(kFlagVisible, v);
    }
    void SetSquirtPending(bool v) {
        SetFlag(kFlagNeedSquirt, v);
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

    // `emissionScaler` multiplies the emission rate. Threaded in from the owning
    // service rather than read from a global, so two scenes can scale
    // independently.
    void Update(f32 elapsed, f32 emissionScaler);

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

    const ParticlePool& Pool() const {
        return pool_;
    }
    ParticlePool& Pool() {
        return pool_;
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


    void InternalUpdate(f32 elapsed, f32 emissionScaler);

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

    u32 flags_ = 0;

    SpawnParams spawn_;
    MotionParams motion_;
    f32 emissionRate_ = 0.0f;

    f32 numNew_ = 0.0f;

    Matrix44f modelToWorld_ = Matrix44f::identity();

    RndSeed randSeed_;
    RndSeed compactSeed_;

    ParticlePool pool_;
};

} // namespace whiteout::flakes::renderer::particle
