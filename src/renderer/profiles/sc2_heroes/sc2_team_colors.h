#pragma once

// ============================================================================
// The recovered StarCraft II team-colour palette.
//
// gamedata.xml <TeamColors> (mods/core.sc2mod): the sixteen melee slots'
// Diffuse / Emissive pairs — exactly what the engine uploads as
// p_cDiffuseTeamColor / p_cEmissiveTeamColor. Display-referred, like every
// editor-authored colour. tc00 White .. tc15 Pink.
//
// A header of its own because two things have to agree on it: the shading that
// draws a unit in its team colour, and the glTF export that has to bake that
// same colour in, glTF having no team slot to fill at runtime.
// ============================================================================

#include "whiteout/flakes/types.h"

namespace whiteout::flakes::renderer::profiles::sc2_heroes {

struct Sc2TeamColor {
    Vector3f diffuse;
    Vector3f emissive;
};

inline constexpr Sc2TeamColor kSc2TeamColors[] = {
    {{1.000000f, 1.000000f, 1.000000f}, {0.764600f, 0.764600f, 0.764600f}}, // White
    {{0.705882f, 0.078431f, 0.117647f}, {0.545098f, 0.145098f, 0.145098f}}, // Red
    {{0.000000f, 0.258700f, 1.000000f}, {0.000000f, 0.258700f, 1.000000f}}, // Blue
    {{0.109800f, 0.654700f, 0.917700f}, {0.066600f, 0.517500f, 0.733300f}}, // Teal
    {{0.301961f, 0.000000f, 0.784314f}, {0.274510f, 0.176471f, 0.627451f}}, // Purple
    {{0.921600f, 0.882300f, 0.161000f}, {0.588100f, 0.588100f, 0.117600f}}, // Yellow
    {{0.996000f, 0.541200f, 0.055000f}, {0.996000f, 0.541200f, 0.055000f}}, // Orange
    {{0.086100f, 0.502000f, 0.000000f}, {0.086100f, 0.502000f, 0.000000f}}, // Green
    {{0.800000f, 0.650980f, 0.988235f}, {0.800000f, 0.650980f, 0.988235f}}, // Light Pink
    {{0.121569f, 0.003922f, 0.788235f}, {0.121569f, 0.003922f, 0.788235f}}, // Violet
    {{0.321569f, 0.329412f, 0.580392f}, {0.078431f, 0.211765f, 0.317647f}}, // Light Grey
    {{0.062700f, 0.384200f, 0.274400f}, {0.062700f, 0.384200f, 0.274400f}}, // Dark Green
    {{0.306000f, 0.164700f, 0.015600f}, {0.306000f, 0.164700f, 0.015600f}}, // Brown
    {{0.588235f, 1.000000f, 0.568627f}, {0.517647f, 1.000000f, 0.309804f}}, // Light Green
    {{0.137255f, 0.137255f, 0.137255f}, {0.058824f, 0.058824f, 0.058824f}}, // Dark Grey
    {{0.898000f, 0.357000f, 0.690100f}, {0.898000f, 0.357000f, 0.690100f}}, // Pink
};

/// The palette pair for an instance's packed RGB (r | g<<8 | b<<16). The host
/// swatch stores one colour, the shader needs the diffuse+emissive pair, so
/// the nearest diffuse wins — an exact palette pick maps exactly, anything
/// else degrades to the closest slot rather than a made-up emissive.
inline const Sc2TeamColor& ResolveSc2TeamColor(u32 packed) {
    const f32 r = static_cast<f32>(packed & 0xFF) / 255.0f;
    const f32 g = static_cast<f32>((packed >> 8) & 0xFF) / 255.0f;
    const f32 b = static_cast<f32>((packed >> 16) & 0xFF) / 255.0f;
    const Sc2TeamColor* best = &kSc2TeamColors[0];
    f32 bestD = 1e9f;
    for (const auto& c : kSc2TeamColors) {
        const f32 dr = c.diffuse.x - r, dg = c.diffuse.y - g, db = c.diffuse.z - b;
        const f32 d = dr * dr + dg * dg + db * db;
        if (d < bestD) {
            bestD = d;
            best = &c;
        }
    }
    return *best;
}

} // namespace whiteout::flakes::renderer::profiles::sc2_heroes
