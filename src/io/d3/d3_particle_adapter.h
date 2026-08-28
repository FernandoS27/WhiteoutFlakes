#pragma once

// ============================================================================
// `.prt` -> d3::EmitterDesc.
//
// The one rule that makes this survive a format revision: every channel is
// keyed by its ENGINE CHANNEL ID, never by its slot index. Shipped assets are
// struct version 180 with all forty paths inline; the Switch binary compiles
// version 213, which moved twelve of them into a side block. The slot order
// differs between the two, the ids do not.
// ============================================================================

#include "renderer/particle/d3_emitter_desc.h"
#include "whiteout/flakes/types.h"

#include <memory>

namespace whiteout::sno::d3::native {
struct Particle;
}

namespace whiteout::flakes::io::d3 {

namespace pd3 = ::whiteout::flakes::renderer::particle::d3;

/// Build the shared, immutable description one `.prt` becomes.
///
/// Never fails: a file whose channels are all empty produces a desc that emits
/// nothing, which is what the engine does with it too.
std::shared_ptr<const pd3::EmitterDesc>
BuildD3EmitterDesc(const ::whiteout::sno::d3::native::Particle& prt, i32 snoId);

} // namespace whiteout::flakes::io::d3
