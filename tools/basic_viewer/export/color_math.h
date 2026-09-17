#pragma once

// ============================================================================
// The colour arithmetic the bakes share: the sRGB transfer both ways, Rec. 709
// luminance, and the clamp-and-quantise back to a byte.
// ============================================================================

#include "whiteout/flakes/types.h"

#include <algorithm>
#include <cmath>

namespace whiteout::flakes::color {

inline f32 SrgbToLinear(f32 v) {
    return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f);
}

/// Clamped to [0, 1] first: a bake's arithmetic overshoots.
inline f32 LinearToSrgb(f32 v) {
    v = std::clamp(v, 0.0f, 1.0f);
    return v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f;
}

inline f32 Rec709Luminance(f32 r, f32 g, f32 b) {
    return 0.2126f * r + 0.7152f * g + 0.0722f * b;
}

/// [0, 1] to a byte, rounded; out-of-range values clamp.
inline u8 UnitToByte(f32 v) {
    return static_cast<u8>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
}

} // namespace whiteout::flakes::color
