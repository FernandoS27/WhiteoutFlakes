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

void generateTurbulenceBasis(u32 seed, f32 timeRandomVariation, std::span<f32> rigidBasis,
                             std::span<f32> spinRate) noexcept;

void rotateTurbulenceBasis(std::span<const f32> rigidBasis, std::span<const f32> spinRate,
                           f32 rotation, std::span<f32> out) noexcept;

Float3 sampleTurbulenceVelocity(const SamplerTurbulence& t, Float3 posWorld, f32 time) noexcept;

}
