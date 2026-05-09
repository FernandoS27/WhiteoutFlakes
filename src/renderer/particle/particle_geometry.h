#pragma once

#include "whiteout/flakes/types.h"
#include "particle2_emitter.h"
#include "types.h"

#include <functional>
#include <vector>

namespace whiteout::flakes::renderer::particle {

using FogSampler = std::function<ImVector(const Vector3f&  )>;

struct BuildGeometryInput {
    const Matrix44f* worldToView = nullptr;
    bool             fogEnabled  = false;
    FogSampler       fogSampler  = nullptr;
};

i32 BuildEmitterGeometry(const Emitter2& emitter,
                         const BuildGeometryInput& in,
                         std::vector<Vertex>& out);

}
