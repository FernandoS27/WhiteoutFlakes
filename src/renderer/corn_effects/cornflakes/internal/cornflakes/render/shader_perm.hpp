#pragma once

#include <cornflakes/interface/core/types.hpp>
#include <cornflakes/interface/render/render_packet.hpp>
#include <cornflakes/interface/render/render_view.hpp>

namespace whiteout::cornflakes {

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

enum class RenderPass : u8 {
    Color = 0,
    Motion = 1,
};

inline constexpr u32 kVsPermCount = 72;
// 3.0.0: 9 outer blocks x 32 inner bits. 2.0.0 had 128 inner bits; the fog
// pair left the index when fog became a runtime constant.
inline constexpr u32 kPsPermCount = 288;

struct ShaderPermKey {
    u32 vsPerm = 0;
    u32 psPerm = 0;
    RenderPass pass = RenderPass::Color;

    bool operator==(const ShaderPermKey&) const = default;
};

ShaderPermKey classifyPopcornPerm(const LayerRendererFlags& flags, RenderPass pass) noexcept;

struct VsPermFields {
    u32 modeIdx;
    u32 uvVariant;
    u32 innerBits;
};
constexpr VsPermFields decodeVsPerm(u32 perm) noexcept {
    return {(perm / 8U) / 3U, (perm / 8U) % 3U, perm % 8U};
}

struct PsPermFields {
    u32 modeIdx;
    u32 uvVariant;
    u32 innerBits;
};
constexpr PsPermFields decodePsPerm(u32 perm) noexcept {
    return {(perm / 32U) / 3U, (perm / 32U) % 3U, perm % 32U};
}

}
