#pragma once

/// @file
/// @brief Lightweight scalar curve sampler keyed by `(t, value)` knots.

#include <cornflakes/interface/core/types.hpp>

#include <span>

namespace whiteout::cornflakes {

/// @brief One curve knot — sample time and scalar value.
struct CurveKnot {
    f32 t;
    f32 value;
};

/// @brief Linearly interpolate `knots` at time `t`; clamps at the endpoints.
f32 sampleCurveScalar(std::span<const CurveKnot> knots, f32 t) noexcept;

} // namespace whiteout::cornflakes
