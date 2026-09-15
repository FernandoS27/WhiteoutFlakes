#pragma once

// ============================================================================
// Format -> EmitterDesc translation, one entry point per source format.
// Everything format-specific (WC3's packed start/mid/end segments, its
// filter-mode numbering, its head/tail type encoding) is confined here; the
// emitter and the geometry builder only ever see an EmitterDesc.
// ============================================================================

#include "renderer/particle/base/emitter_desc.h"
#include "whiteout/flakes/model_types.h"

#include <memory>
#include <span>

namespace whiteout::flakes::renderer::particle {

// WC3 PE2 (`ParticleEmitterConfig`, as produced by IModelSource) — billboards.
std::shared_ptr<const EmitterDesc> DescFromWc3Config(const ParticleEmitterConfig& cfg);

// WC3 PE1 (`PE1EmitterConfig`) — particles that are child models. Same emitter,
// different output; no lifetime curves because nothing about a child model's
// appearance is driven by particle age.
std::shared_ptr<const EmitterDesc>
DescFromWc3ChildModelConfig(const model::PE1EmitterConfig& cfg);

// `.m2` (`M2ParticleEmitterConfig`) — billboards driven by WoW's generators.
// `linearColor` de-gammas the record's display-referred colour keys, which the
// HDR profiles need and the gamma ones must not have.
std::shared_ptr<const EmitterDesc> DescFromM2Config(const M2ParticleEmitterConfig& cfg,
                                                    bool linearColor);

// StarCraft II `PAR_` (`Sc2ParticleEmitterConfig`) — the Family::Sc2 desc, with
// `Init`'s load-time derivations (GPU/CPU motion split OP1, legacy Bezier
// promotions, mid-time clamp). `siblings` resolves `collisionSpawnIndex`'s SPACE
// now, not per collision (design R4); out of range gives the default. Returned
// MUTABLE so the loader can stamp `m3Surface` and `priorityPlane` before SetDesc.
std::shared_ptr<EmitterDesc>
DescFromSc2ParticleConfig(const effects::Sc2ParticleEmitterConfig& cfg,
                          std::span<const effects::Sc2ParticleEmitterConfig> siblings);

// The frame state a trail emitter (M2 RPID) runs on for its whole life. Its
// record's tracks are never walked — the model they came from is never placed —
// so this is @ref M2ParticleEmitterConfig::initial in the shape ApplyState
// wants. The owning emitter patches in unit scale, model alpha and transform.
model::FrameState::ParticleFrameState TrailStateFromM2Config(const M2ParticleEmitterConfig& cfg);

} // namespace whiteout::flakes::renderer::particle
