#pragma once

// Internal particle simulation state. The public ParticleEmitterConfig
// canonically lives in include/whiteout/flakes/model_types.h; we re-include
// it here so existing internal code that references
// `whiteout::flakes::renderer::ParticleEmitterConfig` keeps working.

#include "types.h"
#include "whiteout/flakes/model_types.h"
#include "whiteout/flakes/types.h"

namespace whiteout::flakes::renderer {

struct ParticleEmitterState {
    Matrix44f transform = Matrix44f::identity();
    f32 emissionRate = 0;
    f32 speed = 0;
    f32 variation = 0;
    f32 coneAngle = 0;
    f32 gravity = 0;
    f32 width = 0;
    f32 length = 0;
    f32 visibility = 1.0f;
};

} // namespace whiteout::flakes::renderer
