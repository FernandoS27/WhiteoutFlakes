#pragma once

#include <cornflakes/interface/binding/sampler_resource.hpp>
#include <cornflakes/interface/service/service_types.hpp>

namespace whiteout::cornflakes {

Float3 sampleVectorFieldVelocity(const SamplerVectorField& vf, Float3 posWorld,
                                 const f32* animTime = nullptr) noexcept;

}
