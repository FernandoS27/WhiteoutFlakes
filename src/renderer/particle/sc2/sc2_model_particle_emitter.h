#pragma once

// ============================================================================
// StarCraft II model particles — a `PAR_` with `ModelParticles` set. The SC2
// tick simulates them; the output reaches the actor layer through the Birth /
// Transform / Death channel, with the birth at the pending walk (OP14b), the
// last Update's pose (OP14), and a NaN pose hidden. See SC2_PARTICLE_DESIGN.md §16.5.
// ============================================================================

#include "renderer/particle/base/child_model_emitter.h"

#include <vector>

namespace whiteout::flakes::renderer::particle {

class Sc2ModelParticleEmitter final : public ChildModelEmitter {
public:
    using ChildModelEmitter::ChildModelEmitter;

    /// The queued births and deaths, then one Transform per element holding a
    /// model — over the SC2 store, which the base's pool walk never reaches.
    void CollectOutputEvents(std::vector<ChildModelEvent>& out) override;

protected:
    Matrix44f TransformFor(u32 node) const override;
    f32 VisibilityFor(u32 node) const override;
    u32 PathIndexFor(u32 node) const override;
    /// An `.m3` from the `PAR_`'s table, resolved by the loader like an `.m2`.
    ChildModelEvent::Route BirthRoute() const override {
        return ChildModelEvent::Route::ModelParticle;
    }
};

} // namespace whiteout::flakes::renderer::particle
