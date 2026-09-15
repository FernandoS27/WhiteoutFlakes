#pragma once

// ============================================================================
// Private to the SC2 kernel translation units: what more than one stage's
// .cpp reads and no caller outside them may.
// ============================================================================

#include "sc2_kernel_types.h"
#include "whiteout/flakes/types.h"

#include "renderer/sc2/sc2_newton.h"

namespace whiteout::flakes::renderer::particle::detail {

/// A type-6 instance slower than this (squared) freezes its direction: the
/// Euler step's latch, and the CPU vertex builder's choice of velocity lane.
inline constexpr f32 kFreezeSpeedSq = 0.001f;

} // namespace whiteout::flakes::renderer::particle::detail
