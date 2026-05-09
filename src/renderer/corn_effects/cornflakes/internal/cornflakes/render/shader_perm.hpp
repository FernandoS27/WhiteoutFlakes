#pragma once

/// @file
/// @brief Renderer-feature → VS/PS shader-permutation index mapping.

#include <cornflakes/interface/core/types.hpp>
#include <cornflakes/interface/render/render_packet.hpp>
#include <cornflakes/interface/render/render_view.hpp>

namespace whiteout::cornflakes {

/// @brief Boolean feature flags that select a shader permutation for one renderer.
struct LayerRendererFlags {

    bool hasUV = false;
    bool isBillboard = false;
    bool isAtlas = false;
    bool hasRandom = false;
    bool hasVC = false;
    bool hasNT = false;

    bool writeGBuffer = false;
    bool hasSoftParticles = false;
    bool hasAlphaLut = false;
    bool isLit = false;
};

/// @brief Which scene pass a shader permutation is wanted for.
enum class RenderPass : u8 {
    Color = 0,
    Motion = 1,
};

inline constexpr u32 kVsPermCount = 72;   ///< Total VS permutations (modeIdx*uvVariant*innerBits).
inline constexpr u32 kPsPermCount = 1152; ///< Total PS permutations.

/// @brief Composite key identifying one VS/PS pair for a pass.
struct ShaderPermKey {
    u32 vsPerm = 0;
    u32 psPerm = 0;
    RenderPass pass = RenderPass::Color;

    bool operator==(const ShaderPermKey&) const = default;
};

/// @brief Reduce renderer flags + fog + pass into the corresponding VS/PS perm indices.
ShaderPermKey classifyCornFxPerm(const LayerRendererFlags& flags, FogMode fog,
                                  RenderPass pass) noexcept;

/// @brief Decoded field triple of a VS perm index.
struct VsPermFields {
    u32 modeIdx;
    u32 uvVariant;
    u32 innerBits;
};
constexpr VsPermFields decodeVsPerm(u32 perm) noexcept {
    return {(perm / 8U) / 3U, (perm / 8U) % 3U, perm % 8U};
}

/// @brief Decoded field triple of a PS perm index.
struct PsPermFields {
    u32 modeIdx;
    u32 uvVariant;
    u32 innerBits;
};
constexpr PsPermFields decodePsPerm(u32 perm) noexcept {
    return {(perm / 128U) / 3U, (perm / 128U) % 3U, perm % 128U};
}

} // namespace whiteout::cornflakes
