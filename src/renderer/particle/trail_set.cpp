#include "renderer/particle/trail_set.h"

#include "renderer/particle/particle2_emitter.h"
#include "renderer/particle/particle_constants.h"

#include <algorithm>
#include <cmath>

namespace whiteout::flakes::renderer::particle {

TrailState TrailState::From(const model::FrameState::ParticleFrameState& st) {
    TrailState t;
    t.transform = st.transform;
    t.emissionRate = st.emissionRate;
    t.speed = st.speed;
    t.variation = st.variation;
    t.coneAngle = st.coneAngle;
    t.gravity = st.gravity;
    t.width = st.width;
    t.length = st.length;
    t.visibility = st.visibility;
    t.squirting = st.squirting;
    t.gravityVector = st.gravityVector;
    t.hasGravityVector = st.hasGravityVector;
    t.lifeSpan = st.lifeSpan;
    t.horizontalRange = st.horizontalRange;
    t.zSource = st.zSource;
    t.modelAlpha = st.modelAlpha;
    t.worldPosition = st.worldPosition;
    t.unitScale = st.unitScale;
    return t;
}

TrailSet::TrailSet() = default;
TrailSet::~TrailSet() = default;

void TrailSet::Adopt(std::unique_ptr<Emitter2> trail) {
    if (!trail || trails_.size() >= kMax)
        return;
    // Both bits the adoption is responsible for: the enable the client ORs in
    // (`RecursiveEmitterModelLoaded` @0x1016a6b60), and the marker that keeps it
    // from being cleared again — nothing animates a trail, so nothing would
    // ever set it a second time.
    trail->isTrail_ = true;
    trail->visible_ = true;
    // A trail never emits from its own position, so a squirt it inherited from
    // its record would fire once, from wherever it happens to sit. The client
    // cannot reach that either: the squirt bit is written by AnimateParticleST,
    // which never runs for a trail.
    trail->run_.squirtOwed = false;
    trails_.push_back(std::move(trail));
}

void TrailSet::ApplyOwner(const Emitter2& owner) {
    // Nothing else reaches a trail, so the rest of the per-frame state a trail
    // reads has to arrive the same way — it belongs to the owning actor, not to
    // the model the trail's record came from: the unit scale (which decides how
    // the gravity vector is converted), the model alpha, the transform and the
    // position.
    for (auto& t : trails_) {
        TrailState st = t->trailState_;
        st.unitScale = owner.placement_.unitScale;
        st.modelAlpha = owner.wow_.modelAlpha;
        st.transform = owner.placement_.modelToWorld;
        st.worldPosition = owner.placement_.worldPos;
        t->ApplyAnimated(st);
        // ApplyState is the animation's entry point and clears the enable bit
        // when the track says so; a trail has no track and stays on.
        t->visible_ = true;
        t->wow_.viewDistance = owner.wow_.viewDistance;
    }
}

void TrailSet::Drive(const Emitter2& owner, const Particle2& p, f32 dt, f32 emissionScaler) {
    const EmitterDesc& d = *owner.desc_;
    const Matrix44f& ownerToWorld = owner.placement_.modelToWorld;
    // Where this particle is in world space. A world-space emitter stores that
    // directly; a model-space one stores model units and is transformed at
    // draw, so its trail's spawn point has to go the same way. No shipped
    // record carries a trail on a model-space emitter, so the second case is
    // reasoned, not measured.
    const Vector3f world =
        d.modelSpace ? whiteout::transform_point(p.position, ownerToWorld) : p.position;

    for (auto& t : trails_) {
        // The client stamps the particle's position into the translation row of
        // its OWN particle-to-world matrix, hands that matrix to the trail as
        // the spawn transform, and puts it back afterwards
        // (`UpdateLiveParticle` @0x1016aa080, disassembly at 0x1016aa113 and
        // 0x1016aa176). Restoring matters for the same reason it does there: a
        // trail's DRAW transform must stay the parent model's, not the last
        // particle's.
        const Matrix44f saved = t->placement_.modelToWorld;
        t->placement_.modelToWorld = ownerToWorld;
        t->placement_.modelToWorld.data[3][0] = world.x;
        t->placement_.modelToWorld.data[3][1] = world.y;
        t->placement_.modelToWorld.data[3][2] = world.z;

        // Seeded rather than left where it was. The client never writes a
        // trail's previous position unless the trail asked for random spacing —
        // no shipped one does — so its spawn segment runs from the world origin
        // to the particle and smears across the map. That is uninitialised
        // state, not a behaviour, and seeding prev := cur collapses the segment
        // to the point the feature is for. See M2_TRAIL_EMITTER_DESIGN.md D1.
        t->placement_.worldPos = world;
        t->placement_.prevWorldPos = world;
        t->placement_.seeded = true;

        // The flag consulted is the TRAIL's, not ours.
        if (t->desc_->inheritVelocity) {
            const Vector3f v =
                d.modelSpace ? whiteout::transform_normal(p.velocity, ownerToWorld) : p.velocity;
            const f32 k = t->MotionToParticleSpace();
            t->wow_.motion.emitterVelocity = {v.x * k, v.y * k, v.z * k};
        }

        t->EmitStep(dt, emissionScaler);
        t->placement_.modelToWorld = saved;
    }
}

void TrailSet::Step(f32 dt, f32 emissionScaler) {
    for (auto& t : trails_)
        t->InternalUpdate(dt, emissionScaler);
}

void TrailSet::GrowPools(u32 ownerCapacity) {
    // A trail is driven once per live particle of its owner, so it needs room
    // for its own steady-state population times the owner's — the client's own
    // product, clamped the same way (`Sync` @0x10169f860). Both pools only ever
    // grow, here and in the client (`SyncSize` @0x10169e8c0 "never shrinks"),
    // so the trail's own Sync cannot undo this.
    for (auto& t : trails_) {
        const f32 tLife =
            (std::max)(t->wow_.lifeSpan, 0.0f) + std::fabs(t->desc_->lifespanVariation);
        const u32 own = static_cast<u32>(kPoolHeadroom * t->emissionRate_ * tLife);
        const u64 product = static_cast<u64>(ownerCapacity) * static_cast<u64>(own);
        t->GrowPool(static_cast<u32>(product > kTrailPoolCap ? kTrailPoolCap : product));
    }
}

void TrailSet::Reset() {
    for (auto& t : trails_)
        t->ResetParticles();
}

} // namespace whiteout::flakes::renderer::particle
