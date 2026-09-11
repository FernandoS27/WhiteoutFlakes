#pragma once

// ============================================================================
// GroundQuery — one signature, every consumer.
//
// "Is there a surface under this point, and how high?" was declared three
// times with byte-identical signatures: RenderSettings (the host's override
// point), ribbon::GroundQuery (the SC2 legacy integrator sweeping its
// segments) and d3::Emitter (particle terrain collision). Three spellings of
// one contract is how the three drift apart, and the SC2 particle MOVE stage
// is about to become a fourth caller — so the type lives here and the others
// alias it.
//
// The contract, as the callers already implement it: `up` and `down` bound how
// far the caller will reach from `pos` for a surface. Outside that window the
// answer is false and `outZ` is untouched — a miss, not a zero. The default
// implementation is physics::FlatGroundQuery against the grid
// (physics/ground_plane.h); a host with real terrain installs its own through
// RenderSettings::SetGroundQuery.
// ============================================================================

#include "whiteout/flakes/types.h"

#include <functional>

namespace whiteout::flakes::renderer {

using GroundQuery = std::function<bool(const Vector3f& pos, f32 up, f32 down, f32& outZ)>;

} // namespace whiteout::flakes::renderer
