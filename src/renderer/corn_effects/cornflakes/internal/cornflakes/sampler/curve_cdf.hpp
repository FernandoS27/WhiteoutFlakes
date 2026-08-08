#pragma once

#include <cornflakes/interface/binding/sampler_resource.hpp>
#include <cornflakes/interface/core/arena.hpp>

namespace whiteout::cornflakes {

void buildSamplerCurveCdf(SamplerCurve& curve, IArena& arena) noexcept;

}
