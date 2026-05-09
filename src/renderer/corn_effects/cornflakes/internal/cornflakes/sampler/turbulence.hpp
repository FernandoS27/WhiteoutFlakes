#pragma once

/// @file
/// @brief 3D turbulence/noise scalar sampler used by FX motion modifiers.

#include <cornflakes/interface/core/types.hpp>
#include <cornflakes/interface/service/service_types.hpp>

namespace whiteout::cornflakes {

/// @brief Stateless turbulence parameters; same `seed` always yields the same field.
struct TurbulenceSampler {
    u32 seed = 0U;
    f32 frequency = 1.0F;
    f32 amplitude = 1.0F;
};

/// @brief Evaluate the turbulence field at world position `pos`.
f32 sampleTurbulence3D(const TurbulenceSampler& t, Float3 pos) noexcept;

} // namespace whiteout::cornflakes
