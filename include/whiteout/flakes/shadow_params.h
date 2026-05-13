#pragma once

// ============================================================================
// WhiteoutFlakes — cascade-shadow tuning struct.
//
// Public POD that the shadow service consumes. Pre-existing internal
// references (whiteout::flakes::renderer::shadow::ShadowParams) keep working
// via the using-alias at the bottom.
// ============================================================================

#include "types.h"

namespace whiteout::flakes {

struct ShadowParams {

    i32 cascadeCount = 1;

    i32 cascadeResolution = 1024;

    f32 casterHeight = 200.0f;

    f32 lambdaSplit = 0.5f;

    i32 depthBias = 0;
    f32 slopeScaledBias = 0.0f;
    f32 depthBiasClamp = 0.0f;

    bool texelSnap = true;

    bool enabled = false;
};

} // namespace whiteout::flakes

namespace whiteout::flakes::renderer::shadow {
using ::whiteout::flakes::ShadowParams;
}
