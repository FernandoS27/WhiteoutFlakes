#pragma once

#include "common_types.h"

namespace WhiteoutDex::shadow {

struct ShadowParams {

    i32   cascadeCount       = 1;

    i32   cascadeResolution  = 1024;

    f32   casterHeight       = 200.0f;

    f32   lambdaSplit        = 0.5f;

    i32   depthBias          = 0;
    f32   slopeScaledBias    = 0.0f;
    f32   depthBiasClamp     = 0.0f;

    bool  texelSnap          = true;

    bool  enabled            = false;
};

}
