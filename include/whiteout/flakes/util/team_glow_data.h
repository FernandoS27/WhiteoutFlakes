#pragma once

/// @file team_glow_data.h
/// @brief Synthetic team-glow texture decoder.

#include "../types.h"

#include <vector>

namespace whiteout::flakes::io {

/// @brief Build an RGBA8 team-glow texture tinted with the given colour.
///
/// Decodes the engine's embedded glow TGA and re-tints it to the actor's
/// team colour. Used by the texture manager whenever a model references
/// `TeamGlow*.blp` — the source asset isn't shipped, so we generate it.
///
/// @param tcR,tcG,tcB Team colour in 0..255 sRGB.
/// @param outW,outH Receive the texture dimensions.
/// @return Tightly-packed RGBA8 pixels, `outW * outH * 4` bytes.
std::vector<u8> DecodeTeamGlow(u8 tcR, u8 tcG, u8 tcB, i32& outW, i32& outH);

} // namespace whiteout::flakes::io

namespace whiteout::flakes {
using ::whiteout::flakes::io::DecodeTeamGlow;
}
