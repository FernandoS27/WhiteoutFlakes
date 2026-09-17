#pragma once

// ============================================================================
// An 8-bit RGB colour and the two ways the host packs one into an integer.
//
// Both orders are in use and neither can change: RenderSettings'
// BackgroundColorRaw and Actor::teamColor are 0x00BBGGRR, while the settings
// file stores FogColor and Export.Background as 0x00RRGGBB.
// ============================================================================

#include "whiteout/flakes/types.h"

#include <algorithm>
#include <array>

namespace whiteout::flakes::tools {

struct Rgb8 {
    u8 r = 0;
    u8 g = 0;
    u8 b = 0;
};

/// 0x00BBGGRR.
constexpr Rgb8 UnpackBgr(u32 packed) noexcept {
    return {static_cast<u8>(packed & 0xFFu), static_cast<u8>((packed >> 8) & 0xFFu),
            static_cast<u8>((packed >> 16) & 0xFFu)};
}

/// 0x00RRGGBB.
constexpr Rgb8 UnpackRgb(u32 packed) noexcept {
    return {static_cast<u8>((packed >> 16) & 0xFFu), static_cast<u8>((packed >> 8) & 0xFFu),
            static_cast<u8>(packed & 0xFFu)};
}

constexpr u32 PackRgb(Rgb8 c) noexcept {
    return (static_cast<u32>(c.r) << 16) | (static_cast<u32>(c.g) << 8) | static_cast<u32>(c.b);
}

/// For a colour picker: each channel in [0, 1].
inline std::array<f32, 3> ToUnitRgb(Rgb8 c) noexcept {
    return {static_cast<f32>(c.r) / 255.0f, static_cast<f32>(c.g) / 255.0f,
            static_cast<f32>(c.b) / 255.0f};
}

/// Back from a picker, truncating: what the background and team colour
/// swatches have always stored.
inline Rgb8 FromUnitRgbTruncated(const f32 rgb[3]) noexcept {
    return {static_cast<u8>(rgb[0] * 255.0f), static_cast<u8>(rgb[1] * 255.0f),
            static_cast<u8>(rgb[2] * 255.0f)};
}

/// Back from a picker, clamped and rounded: what the fog colour stores.
inline Rgb8 FromUnitRgbRounded(const f32 rgb[3]) noexcept {
    const auto channel = [](f32 v) {
        return static_cast<u8>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
    };
    return {channel(rgb[0]), channel(rgb[1]), channel(rgb[2])};
}

} // namespace whiteout::flakes::tools
