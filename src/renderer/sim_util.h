#pragma once

#include "common_types.h"
#include "constants.h"
#include <algorithm>

namespace WhiteoutDex {

inline f32 ClampDeltaTime(f32 dt) {
    return std::clamp(dt, 0.0f, kMaxSimulationDt);
}

inline bool IsEmitterVisible(f32 visibility) {
    return visibility > kMinVisibilityThreshold;
}

}
