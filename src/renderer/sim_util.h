#pragma once

#include <algorithm>
#include "constants.h"
#include "whiteout/flakes/types.h"

namespace whiteout::flakes::renderer {

inline f32 ClampDeltaTime(f32 dt) {
    return std::clamp(dt, 0.0f, kMaxSimulationDt);
}

inline bool IsEmitterVisible(f32 visibility) {
    return visibility > kMinVisibilityThreshold;
}

} // namespace whiteout::flakes::renderer
