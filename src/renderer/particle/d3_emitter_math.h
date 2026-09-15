#pragma once

// ============================================================================
// The angle constants the D3 emitter's translation units share. Private to
// them: `d3_emitter.cpp`, `d3_emit_shape.cpp`, `d3_birth.cpp`, `d3_motion.cpp`.
// ============================================================================

#include "io/d3/d3_types.h"
#include "types.h"

namespace whiteout::flakes::renderer::particle::d3::detail {

/// @brief The engine's OTHER two-pi, and the one every azimuth is built from.
///
/// `6.28318452835083` @0x7100E3BFD0, two ulps below the correctly-rounded 2pi it
/// keeps at 0x7100E3BEE4 for angle wrapping. Both print as `6.2832` in a
/// decompile, so nothing but running the code separates them. The three shape
/// samplers multiply their draw by THIS one; `WrapAngle` and `AsinFast` use the
/// real one. Measured as ~4e-7 relative on a sphere sample's x and y.
inline constexpr f32 kAzimuthTwoPi = 6.28318452835083f;

/// @brief The engine's wrap, clamping to 8x2pi before it folds into [0, 2pi], so an
///        angle that ran away is pinned rather than folded from wherever it reached.
///
/// `50.26548386`, which is `8 * 2pi` in single precision — NOT the `50.265` a
/// decompile prints. Hex-Rays rounds a float constant to five significant digits
/// when it displays it, so a constant read out of pseudocode is a rounded
/// reading of the real one; this is the same trap that had AsinFast's whole
/// polynomial off by 3e-5. Kept because the clamp changes the result for a large
/// angle, not just the speed.
inline f32 WrapAngle(f32 a) {
    return ::whiteout::flakes::io::D3WrapAngle(a);
}

} // namespace whiteout::flakes::renderer::particle::d3::detail
