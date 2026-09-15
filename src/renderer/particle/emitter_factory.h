#pragma once

// ============================================================================
// EmitterFactory — the one place a particle emitter is made.
//
// Class choice, describe, behaviour and seed, in that order, inside one
// function. The order is load-bearing: `SetSeed` draws the random flipbook
// start off the emitter's stream when the DESC asks for one, so a seed set
// before the desc skips a draw the desc wanted. Each registration site used to
// spell that order itself.
//
// The behaviour stays a registration argument rather than a desc field (the
// ribbon's choice): a particle desc is shared by every actor of a template,
// and the behaviour is the load-time profile's, not the model's.
// ============================================================================

#include "core/particle_dialect.h"
#include "particle_output.h"
#include "types.h"
#include "whiteout/flakes/types.h"

#include <functional>
#include <memory>

namespace whiteout::flakes::renderer::particle {

class Emitter2;
struct EmitterDesc;

namespace d3 {
class Emitter;
struct EmitterDesc;
} // namespace d3

/// Who a child-model output's children belong to, and what mints their handles
/// — routed through the scene's id allocator by the caller, so the renderer
/// never exposes a mutable counter. Read only for an emitter whose output is
/// `ParticleOutput::ChildModel`.
struct ChildModelOwner {
    ModelId owner = 0;
    i32 emitterId = 0;
    std::function<u32()> allocHandle;
};

namespace EmitterFactory {

/// A WC3, WoW or SC2 emitter for @p desc, of the class its output and family
/// call for: `Emitter2` for billboards, `ChildModelEmitter` for a PE1,
/// `ModelParticleEmitter` for an M2 model particle, `Sc2ModelParticleEmitter`
/// for an SC2 one. Described, then given @p behavior — an SC2 emitter keeps the
/// default, because the WC3-family tuning selects nothing in its stages — then
/// seeded.
std::unique_ptr<Emitter2> Create(std::shared_ptr<const EmitterDesc> desc,
                                 const core::ParticleBehavior& behavior, u32 seed,
                                 ChildModelOwner child = {});

/// A Diablo III emitter riding @p bone at @p offset. Its children report to
/// @p child when it carries an allocator; without one a child-actor system
/// counts its emissions and spawns nothing.
std::unique_ptr<d3::Emitter> CreateD3(std::shared_ptr<const d3::EmitterDesc> desc, i32 bone,
                                      const Matrix44f& offset, ChildModelOwner child = {});

} // namespace EmitterFactory

} // namespace whiteout::flakes::renderer::particle
