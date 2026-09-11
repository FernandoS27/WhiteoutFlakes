#pragma once

// ============================================================================
// Format -> EmitterDesc translation.
//
// One entry point per source format. Everything format-specific (WC3's packed
// start/mid/end segments, its filter-mode numbering, its head/tail particle
// type encoding) is confined here; the emitter and the geometry builder only
// ever see an EmitterDesc.
//
// M2 and M3 adapters land beside DescFromWc3Config when those formats arrive.
// ============================================================================

#include "emitter_desc.h"
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

// StarCraft II `PAR_` (`Sc2ParticleEmitterConfig`) — the Family::Sc2 desc,
// including the load-time derivations `Init` performs: the GPU/CPU motion
// split (OP1), the legacy Bezier promotions, and the mid-time clamp.
//
// `siblings` is the whole emitter list this one came from, and it is here for
// one reason: `collisionSpawnIndex` names another emitter whose SPACE the MOVE
// stage needs, and resolving it now is what keeps that stage from looking an
// emitter up per collision (design R4). Out-of-range indices resolve to the
// default rather than being dropped — the child link is the loader's business.
// Returned MUTABLE, unlike its siblings: the loader stamps `m3Surface` and
// `priorityPlane` from the surface table before handing it to SetDesc, which
// takes it as const. Freezing it here would only buy a const_cast there.
std::shared_ptr<EmitterDesc>
DescFromSc2ParticleConfig(const effects::Sc2ParticleEmitterConfig& cfg,
                          std::span<const effects::Sc2ParticleEmitterConfig> siblings);

// The frame state a trail emitter (M2 RPID) runs on for its whole life. Its
// record's tracks are never walked — the model they came from is never placed —
// so this is @ref M2ParticleEmitterConfig::initial in the shape ApplyState
// wants. The owning emitter patches in unit scale, model alpha and transform.
model::FrameState::ParticleFrameState TrailStateFromM2Config(const M2ParticleEmitterConfig& cfg);

} // namespace whiteout::flakes::renderer::particle
