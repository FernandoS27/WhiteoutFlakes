#pragma once

/// @file shadow_params.h
/// @brief Public POD struct that tunes the renderer's cascade shadow map.

#include "types.h"

namespace whiteout::flakes {

/// @brief Cascade-shadow-map tuning consumed by `ShadowService`.
///
/// `enabled = false` disables shadows entirely (no shadow pass, no
/// sampler binds). When enabled, `cascadeCount` cascades of
/// `cascadeResolution × cascadeResolution` each are rendered every
/// frame; `lambdaSplit` mixes uniform and logarithmic cascade
/// distribution (0 = uniform, 1 = log).
struct ShadowParams {
    /// Number of cascades; the renderer accepts 1..3.
    i32 cascadeCount = 1;
    /// Per-cascade shadow-map side length in texels.
    i32 cascadeResolution = 1024;
    /// Extruded receiver-height padding for the cascade frustums; larger
    /// values include taller casters in the cascade at a quality cost.
    f32 casterHeight = 200.0f;
    /// Cascade-distribution lambda: 0 = uniform splits, 1 = logarithmic.
    f32 lambdaSplit = 0.5f;

    /// @name Depth-bias tuning (passed to the rasterizer state).
    /// @{
    i32 depthBias = 0;
    f32 slopeScaledBias = 0.0f;
    f32 depthBiasClamp = 0.0f;
    /// @}

    /// Snap cascade origin to texel grid to suppress shimmering as the
    /// camera moves.
    bool texelSnap = true;

    /// Master enable. Off ⇒ no shadow pass; the lit pass binds a 1×1
    /// white sampler so PCF reads return "lit".
    bool enabled = false;
};

} // namespace whiteout::flakes

namespace whiteout::flakes::renderer::shadow {
using ::whiteout::flakes::ShadowParams;
}
