#pragma once

// ============================================================================
// StarCraft II model particles — a `PAR_` with `ModelParticles` set.
//
// The simulation is the SC2 tick's, unchanged: these elements live in the
// runtime's store like every other SC2 particle. Only the output differs, and
// it reaches the actor layer through the Birth / Transform / Death channel PE1
// and M2 already use. Three things about it are SC2's own:
//
//  - a birth happens when the frame's pending walk reaches the element
//    (`ProcessPendingSpawns`, OP14b), not when it spawns, and carries the
//    `childModelPaths` index that walk drew;
//  - the transform is the pose `UpdateModelParticle` (OP14) wrote at the
//    element's last Update, not one recomputed at collection;
//  - a pose the kernel hands back as NaN — types 5 and 6 with a zero
//    `instanceAngle` — is hidden here rather than clamped inside the kernel,
//    so the kernel stays comparable against its golden.
//
// Not carried, as for M2: the per-instance tint and alpha, and the fog-of-war
// visibility colour (RE §6.1). The actor layer has no channel for them.
// ============================================================================

#include "child_model_emitter.h"

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
};

} // namespace whiteout::flakes::renderer::particle
