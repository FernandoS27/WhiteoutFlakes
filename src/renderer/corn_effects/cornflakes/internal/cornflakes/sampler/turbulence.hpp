#pragma once

#include <cornflakes/interface/binding/sampler_resource.hpp>
#include <cornflakes/interface/core/types.hpp>
#include <cornflakes/interface/service/service_types.hpp>

#include <span>

namespace whiteout::cornflakes {

struct TurbulenceSampler {
    u32 seed = 0U;
    f32 frequency = 1.0F;
    f32 amplitude = 1.0F;
};

f32 sampleTurbulence3D(const TurbulenceSampler& t, Float3 pos) noexcept;

inline constexpr std::size_t kTurbulenceGradientCount = 256;

void generateTurbulenceGradients(u32 seed, std::span<f32> out) noexcept;

Float3 sampleTurbulenceVelocity(const SamplerTurbulence& t, Float3 pos, f32 time) noexcept;

}
