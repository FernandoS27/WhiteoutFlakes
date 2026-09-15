#pragma once

// ============================================================================
// Private to the SC2 kernel translation units: what more than one stage's
// .cpp reads and no caller outside them may.
// ============================================================================

#include "sc2_kernel_types.h"
#include "whiteout/flakes/types.h"

#if defined(__SSE__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 1)
#define WDX_SC2_HAS_RCPSS 1
#include <xmmintrin.h>
#else
#define WDX_SC2_HAS_RCPSS 0
#endif

namespace whiteout::flakes::renderer::particle::detail {

/// `a·b` left to right — the grouping the CPU vertex builder's tail clamp and
/// velocity test use (OP11). The simulate step groups its own dot products
/// differently and spells them out where they are.
inline f32 EulerDot(const Vector3f& a, const Vector3f& b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

/// A type-6 instance slower than this (squared) freezes its direction: the
/// Euler step's latch, and the CPU vertex builder's choice of velocity lane.
inline constexpr f32 kFreezeSpeedSq = 0.001f;

} // namespace whiteout::flakes::renderer::particle::detail
