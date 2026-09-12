#pragma once

/// @file team_glow_data.h
/// @brief Synthetic team-glow texture decoder.

#include "../types.h"

#include <vector>

namespace whiteout::flakes::io {

/// Side of the generated glow, in pixels. The embedded art is 256 and its
/// disc only 216 of that; the shape is a radial ramp, so it can be redrawn
/// as large as wanted without inventing detail.
constexpr i32 kTeamGlowSize = 512;

/// @brief Build an RGBA8 team-glow texture tinted with the given colour.
///
/// Re-draws the engine's embedded glow TGA at `size` and tints it to the
/// actor's team colour. Used by the texture manager whenever a model
/// references `TeamGlow*.blp` — the source asset isn't shipped, so we
/// generate it. The glow's shape rides the RGB over a flat alpha, which is
/// how the Warcraft III asset carries it.
///
/// @param tcR,tcG,tcB Team colour in 0..255 sRGB.
/// @param outW,outH Receive the texture dimensions.
/// @param size Side in pixels; `0` keeps the embedded art's own.
/// @return Tightly-packed RGBA8 pixels, `outW * outH * 4` bytes.
std::vector<u8> DecodeTeamGlow(u8 tcR, u8 tcG, u8 tcB, i32& outW, i32& outH,
                               i32 size = kTeamGlowSize);

} // namespace whiteout::flakes::io

namespace whiteout::flakes {
using ::whiteout::flakes::io::DecodeTeamGlow;
}
