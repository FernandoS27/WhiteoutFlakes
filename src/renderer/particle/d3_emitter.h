#pragma once

// ============================================================================
// d3::Emitter — the Diablo III particle simulation.
//
// A third dialect, not a third set of curves. It plugs in at Emitter2's two
// virtual seams (Update and BuildGeometry) and shares the pool, the registry,
// the draw list and the transparent sort with WC3 and WoW; everything above
// those is its own. See D3_PARTICLE_DESIGN.md §12.
//
// Two facts shape the whole class:
//
//  * For an ordinary system (`eSystemType` 0 or 10, 77% of the corpus) motion
//    is 100% authored channels. `ParticleSystem_UpdateParticles` seeds its
//    displacement accumulator at zero and only the five gated models add to
//    it — there is no gravity, no drag, and the birth velocity from channel 38
//    is not consulted at all. `Particle_ComputeInitialVelocity` is called only
//    for the three types that own a body in the world-collision solver.
//  * `eSystemType` picks the whole update path. Three values are never
//    simulated, two run only a wind spring, three want a world solver. What
//    this class implements is the particle simulation plus an honest
//    degradation for the rest — each one named at its site.
// ============================================================================

#include "d3_emit_mesh.h"
#include "d3_emitter_desc.h"
#include "d3_particle.h"
#include "d3_path.h"
#include "particle2_emitter.h"
#include "types.h"
#include "whiteout/flakes/types.h"

#include <functional>
#include <memory>
#include <span>
#include <vector>

namespace whiteout::flakes::renderer::particle::d3 {

class Emitter final : public particle::Emitter2 {
public:
    Emitter();

    /// Never null: an emitter without one reads a static default whose channels
    /// are all empty, which emits nothing rather than crashing.
    void SetD3Desc(std::shared_ptr<const EmitterDesc> desc);
    const EmitterDesc& D3Desc() const {
        return *d3desc_;
    }

    void Update(f32 elapsed, f32 emissionScaler) override;
    i32 BuildGeometry(const BuildGeometryInput& in, std::vector<Vertex>& out) const override;
    void ApplyState(const model::FrameState::ParticleFrameState& st) override;
    void CollectOutputEvents(std::vector<ChildModelEvent>& out) override;

    /// @brief Who the children belong to, and what mints their handles.
    ///
    /// Same seam ChildModelEmitter takes, and for the same reason: the emitter
    /// reports births as data and the actor layer owns the spawn. Without an
    /// allocator such a system counts its emissions and produces nothing, which
    /// is the right answer for a test that only wants the simulation.
    void SetChildOwner(u32 owner, i32 emitterId, std::function<u32()> alloc) {
        childOwner_ = owner;
        childEmitterId_ = emitterId;
        allocHandle_ = std::move(alloc);
    }

    /// How many child actors this system has spawned. The engine's emit clamp
    /// counts these and not particles (`sys+408 + sys+376`), which is what makes
    /// a target count of 1 — 4,189 of 4,795 shipped files — mean "one model".
    i32 ChildCount() const {
        return static_cast<i32>(childHandles_.size());
    }

    /// The system's own age, which is the clock every layer's UV transform is
    /// sampled against.
    f32 MaterialTimeSec() const override {
        return systemAge_;
    }

    /// The emitter's orientation. Frozen onto each particle at birth and used
    /// for two things only: rotating the birth velocity, and rotating the
    /// emitter-local kinematic triple (§5.1). Hosts that have no orientation
    /// leave it at identity, which makes triple B world-space — the same
    /// answer the engine gives for an unrotated emitter.
    void SetEmitterOrientation(const Quaternion& q) {
        emitterQuat_ = q;
    }

    /// @brief The model surface emitter shapes 6, 7 and 11 sample.
    ///
    /// Two halves because they change at different rates: the geometry is
    /// built once per actor and shared, the pose arrives every frame. Without
    /// either the three shapes fall back to the point case — the engine's own
    /// answer when its actor lookup fails.
    void SetEmitMesh(std::shared_ptr<const EmitMesh> mesh) {
        emitMesh_ = std::move(mesh);
    }
    bool HasEmitMesh() const {
        return emitMesh_ && !emitMesh_->Empty();
    }
    /// @param pose     Node world matrices, model space, by global bone index.
    /// @param invBind  The matching inverse bind matrices. Skinning is
    ///                 `rest * invBind[b] * pose[b]`, the same product the
    ///                 bone palette uploads.
    /// @param toWorld  Model space -> renderer units, scale included.
    void SetEmitMeshPose(std::span<const Matrix44f> pose, std::span<const Matrix44f> invBind,
                         const Matrix44f& toWorld) {
        emitPose_ = pose;
        emitInvBind_ = invBind;
        emitMeshToWorld_ = toWorld;
    }

    /// The bound spawn target the seek model converges on. Without one the
    /// model is inert, which is what the engine does when nothing is bound.
    void SetSeekTarget(const Vector3f& t) {
        seekTarget_ = t;
        hasSeekTarget_ = true;
    }
    void ClearSeekTarget() {
        hasSeekTarget_ = false;
    }

    /// Global wind, for the two foliage system types. `phase` is a shared
    /// clock; each particle offsets it by its own random so a gust travels.
    void SetWind(const Vector2f& dir, f32 strength, f32 phase) {
        windDir_ = dir;
        windStrength_ = strength;
        windPhase_ = phase;
    }

    /// Which of the model's bones this emitter rides, and where on it.
    ///
    /// A `.prt` reaches a Diablo III model as a TriggerEvent naming a
    /// *hardpoint* (see `io/d3/d3_effect_resolver.h`), and a hardpoint is a
    /// bone index plus a local frame — so the placement is that frame composed
    /// onto the bone's world matrix, and nothing else. There are no per-frame
    /// emitter tracks of the kind `.mdx` and `.m2` carry. A bone of -1 rides
    /// the model origin, which is what `Default` resolves to.
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

    /// Restart the system: age, accumulator and pool. Used when an actor
    /// re-triggers an effect rather than respawning it.
    void Restart();

protected:
    void OnPoolResized(usize capacity) override;

private:
    /// What `ParticleSystem_TickEmitter` precomputes once per tick and
    /// `ParticleSystem_SampleEmitterShape` reads per particle.
    struct EmitContext {
        Shape shape = Shape::Point;
        Vector3f offset{0, 0, 0};
        f32 ext0Lo = 0.0f, ext0Span = 0.0f;
        f32 ext1Lo = 0.0f, ext1Span = 0.0f;
        Vector3f boxLo{0, 0, 0};
        Vector3f boxHi{0, 0, 0};
        i32 emitCount = 0;
        i32 emitIndex = 0;
        f32 ringPhase = 0.0f;
    };

    /// One area-weighted (or, for shape 11, sequential) point on @ref
    /// emitMesh_, skinned and taken to renderer units. False leaves the caller
    /// on the point case.
    bool SampleEmitMeshPoint(bool sequential, u32 sequence, Vector3f& out);
    Vector3f SkinEmitMeshVertex(const EmitMesh& m, u32 vertex) const;

    EvalCtx EmitterCtx() const;
    EvalCtx ParticleCtx(const ParticleState& st, const Vector3f& pos, f32 age) const;

    void TickEmit(f32 dt, f32 emissionScaler);
    EmitContext BuildEmitContext() const;
    Vector3f SampleShape(EmitContext& ec, const Vector3f& base);
    bool BirthParticle(f32 dt, EmitContext& ec);
    bool SpawnChildActor(EmitContext& ec);
    void StepParticle(u32 idx, f32 dt);
    void StepWindSpring(f32 dt);

    std::shared_ptr<const EmitterDesc> d3desc_;
    std::vector<ParticleState> states_;

    /// The system's own random stream — particle seeds and the shape draws
    /// come out of it. Distinct from `Emitter2::randSeed_`, which is the
    /// WC3/WoW `CRandom` and is never touched here.
    MwcRng sysRng_{0x1234567u, 666u};
    /// The seed emitter-side channel evaluations use (`sys+28` in the engine).
    u32 emitterSeed_ = 1;

    f32 systemAge_ = 0.0f;
    f32 emitElapsed_ = 0.0f;
    f32 emitAccum_ = 0.0f;
    f32 lifetimeScale_ = 1.0f;
    i32 emittedLastUpdate_ = 0;
    u32 emitSequence_ = 0; ///< shape 11's monotone counter; never reset

    std::shared_ptr<const EmitMesh> emitMesh_;
    std::span<const Matrix44f> emitPose_;
    std::span<const Matrix44f> emitInvBind_;
    Matrix44f emitMeshToWorld_ = Matrix44f::identity();

    i32 attachBone_ = -1;
    Matrix44f attachOffset_ = Matrix44f::identity();
    Quaternion emitterQuat_ = Quaternion::identity();
    Vector3f seekTarget_{0, 0, 0};
    bool hasSeekTarget_ = false;

    Vector2f windDir_{1, 0};
    f32 windStrength_ = 0.0f;
    f32 windPhase_ = 0.0f;

    // The child actors a type 1/3/4 system has spawned, and the events not yet
    // drained. Handles rather than pool indices: these are not particles, and
    // the engine keeps them in a list of its own for exactly the same reason.
    std::function<u32()> allocHandle_;
    u32 childOwner_ = 0;
    i32 childEmitterId_ = 0;
    std::vector<u32> childHandles_;
    std::vector<ChildModelEvent> childPending_;
};

/// The five random draws the shape sampler makes, exposed so the shape gate
/// can assert on the distribution without standing up a whole emitter.
/// @{
Vector3f SamplePointOnSphere(MwcRng& rng, f32 radius);
Vector3f SamplePointOnHemisphere(MwcRng& rng, f32 radius);
Vector3f SamplePointOnCircleXY(MwcRng& rng, f32 radius);
f32 SampleRadiusInAnnulus(MwcRng& rng, f32 inner, f32 thickness);
/// @}

} // namespace whiteout::flakes::renderer::particle::d3
