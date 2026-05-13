#pragma once

#include "whiteout/flakes/types.h"

namespace whiteout::flakes::renderer::particle {

enum class FilterMode : u8 { Blend = 0, Additive = 1, Modulate = 2, Modulate2X = 3, AlphaKey = 4 };

struct ParticleMaterialDesc {
    i32 textureId = -1;
    FilterMode filterMode = FilterMode::Blend;
    bool unshaded = false;
    bool unfogged = false;
    i32 replaceableId = 0;
};

} // namespace whiteout::flakes::renderer::particle
