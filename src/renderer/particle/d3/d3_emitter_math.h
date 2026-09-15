#pragma once

// ============================================================================
// The angle constants the D3 emitter's translation units share. Private to
// them: `d3_emitter.cpp`, `d3_emit_shape.cpp`, `d3_birth.cpp`, `d3_motion.cpp`.
// ============================================================================

#include "io/d3/d3_types.h"
#include "types.h"

namespace whiteout::flakes::renderer::particle::d3::detail {

/// @brief The engine's OTHER two-pi, and the one every azimuth is built from:
///        `6.28318452835083` @0x7100E3BFD0, two ulps below the correctly-rounded
///        2pi @0x7100E3BEE4 that `WrapAngle` and `AsinFast` use (§20.1, §30.14).
inline constexpr f32 kAzimuthTwoPi = 6.28318452835083f;

/// @brief The engine's wrap, clamping to 8x2pi (`50.26548386`, not the printed
///        `50.265`) before it folds into [0, 2pi], so an angle that ran away is
///        pinned rather than folded from wherever it reached (§20.1, §30.14).
inline f32 WrapAngle(f32 a) {
    return ::whiteout::flakes::io::D3WrapAngle(a);
}

} // namespace whiteout::flakes::renderer::particle::d3::detail
