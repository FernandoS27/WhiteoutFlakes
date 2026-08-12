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

namespace whiteout::flakes::renderer::particle {

// WC3 PE2 (`ParticleEmitterConfig`, as produced by IModelSource) — billboards.
std::shared_ptr<const EmitterDesc> DescFromWc3Config(const ParticleEmitterConfig& cfg);

// WC3 PE1 (`PE1EmitterConfig`) — particles that are child models. Same emitter,
// different output; no lifetime curves because nothing about a child model's
// appearance is driven by particle age.
std::shared_ptr<const EmitterDesc>
DescFromWc3ChildModelConfig(const model::PE1EmitterConfig& cfg);

} // namespace whiteout::flakes::renderer::particle
