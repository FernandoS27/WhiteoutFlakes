#pragma once

// ============================================================================
// Child-model particles — particles that ARE models (MDX PE1).
//
// Same sim as a billboard emitter: same pool, same integration, same
// deterministic RNG. Only the *output* differs, and it is expressed through the
// birth/death hooks rather than a parallel subsystem.
//
// The emitter never touches actors. It reports Birth / Transform / Death as
// data, which FrameTicker drains and turns into SpawnChild / worldTransform /
// DestroyActor. That keeps actor-tree policy (depth and instance caps, asset
// resolution) at the actor layer, and avoids re-entering the service mutex
// through DestroyActor -> ParticleService::RemoveModel.
// ============================================================================

#include "particle2_emitter.h"
#include "particle_service.h"

#include <functional>
#include <vector>

namespace whiteout::flakes::renderer::particle {

class ChildModelEmitter : public Emitter2 {
public:
    // `allocHandle` mints a fresh ActorId per birth — routed through
    // SceneManager::AllocActorId by the caller so the renderer does not expose
    // a mutable counter.
    using HandleAllocator = std::function<u32()>;

    ChildModelEmitter(ModelId owner, i32 emitterId, HandleAllocator allocHandle);

    // PE1's frame state carries latitude/longitude rather than a plane extent,
    // and animates longitude, which PE2 never does.
    void ApplyPE1State(const model::FrameState::PE1FrameState& st);

    void CollectOutputEvents(std::vector<ChildModelEvent>& out) override;

protected:
    void OnPoolResized(usize capacity) override;
    void OnParticleBorn(u32 poolIndex) override;
    void OnParticleDied(u32 poolIndex) override;

    // Where the child actor is put this frame. Keyed by pool index rather than
    // by particle so an override can reach per-particle state alongside the
    // pool — PE1 needs none, an M2 model particle needs its orientation.
    virtual Matrix44f TransformFor(u32 poolIndex) const;

    // Whether the child should draw this frame. Only twinkle answers anything
    // but 1, and only M2 has twinkle.
    virtual f32 VisibilityFor(u32 poolIndex) const {
        return 1.0f;
    }

private:
    ModelId owner_;
    i32 emitterId_;
    HandleAllocator allocHandle_;

    // Index-parallel with the pool; Particle2 is frozen at 32 bytes so the
    // handle cannot live inside it. 0 means "no live child".
    std::vector<u32> childHandles_;

    std::vector<ChildModelEvent> pending_;
};

} // namespace whiteout::flakes::renderer::particle
