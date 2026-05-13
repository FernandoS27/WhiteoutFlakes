#pragma once

#include "types.h"
#include "whiteout/flakes/types.h"

namespace whiteout::flakes::renderer::particle {

struct Particle2 {
    Vector3f position{0, 0, 0};
    u32 keyFrame = 0;
    Vector3f velocity{0, 0, 0};
    f32 age = 0.0f;
};

static_assert(sizeof(Particle2) == 32, "Particle2 must be 32 bytes to mirror CParticle2");

} // namespace whiteout::flakes::renderer::particle
