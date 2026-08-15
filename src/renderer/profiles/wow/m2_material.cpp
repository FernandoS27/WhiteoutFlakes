#include "m2_material.h"

namespace whiteout::flakes::renderer::profiles::wow {
namespace {

using BF = gfx::BlendFactor;

// EGxBlend, reached through s_gxBlend = {0, 1, 2, 10, 3, 4, 5, 13}. Separate
// alpha factors matter: Add writes Zero/One so an additive splat cannot erase
// the destination alpha a later blend reads.
constexpr gfx::BlendDesc kBlendTable[static_cast<usize>(M2Blend::Count)] = {
    // Opaque, AlphaKey — blending off; AlphaKey differs only in its alpha test.
    {.enable = false},
    {.enable = false},
    // Alpha — GxBlend_Alpha
    {.enable = true,
     .srcColor = BF::SrcAlpha,
     .dstColor = BF::InvSrcAlpha,
     .srcAlpha = BF::One,
     .dstAlpha = BF::InvSrcAlpha},
    // NoAlphaAdd — GxBlend_NoAlphaAdd. Colour already carries its intensity.
    {.enable = true,
     .srcColor = BF::One,
     .dstColor = BF::One,
     .srcAlpha = BF::Zero,
     .dstAlpha = BF::One},
    // Add — GxBlend_Add
    {.enable = true,
     .srcColor = BF::SrcAlpha,
     .dstColor = BF::One,
     .srcAlpha = BF::Zero,
     .dstAlpha = BF::One},
    // Mod — GxBlend_Mod
    {.enable = true,
     .srcColor = BF::DstColor,
     .dstColor = BF::Zero,
     .srcAlpha = BF::DstAlpha,
     .dstAlpha = BF::Zero},
    // Mod2x — GxBlend_Mod2x
    {.enable = true,
     .srcColor = BF::DstColor,
     .dstColor = BF::SrcColor,
     .srcAlpha = BF::DstAlpha,
     .dstAlpha = BF::SrcAlpha},
    // BlendAdd — GxBlend_BlendAdd, i.e. premultiplied "over".
    {.enable = true,
     .srcColor = BF::One,
     .dstColor = BF::InvSrcAlpha,
     .srcAlpha = BF::One,
     .dstAlpha = BF::InvSrcAlpha},
};

// s_fogModeList. Indexed by M2BLEND, not EGxBlend — the values only line up
// that way, which the wiki does not say.
constexpr M2FogMode kFogTable[static_cast<usize>(M2Blend::Count)] = {
    M2FogMode::FogColor,  // Opaque
    M2FogMode::FogColor,  // AlphaKey
    M2FogMode::FogColor,  // Alpha
    M2FogMode::Black,     // NoAlphaAdd
    M2FogMode::Black,     // Add
    M2FogMode::White,     // Mod
    M2FogMode::HalfWhite, // Mod2x
    M2FogMode::Black,     // BlendAdd
};

} // namespace

M2Blend M2BlendFromRaw(u16 blendingMode) {
    return blendingMode < static_cast<u16>(M2Blend::Count) ? static_cast<M2Blend>(blendingMode)
                                                           : M2Blend::Opaque;
}

gfx::BlendDesc M2BlendDesc(M2Blend mode) {
    return kBlendTable[static_cast<usize>(mode)];
}

f32 M2AlphaRef(M2Blend mode, f32 elementAlpha) {
    switch (mode) {
    case M2Blend::Opaque:
    case M2Blend::BlendAdd:
        return 0.0f;
    case M2Blend::AlphaKey:
        return kM2AlphaKeyRef * elementAlpha;
    default:
        return kM2DefaultAlphaRef;
    }
}

M2FogMode M2FogModeFor(M2Blend mode) {
    return kFogTable[static_cast<usize>(mode)];
}

bool M2LightingEnabled(M2Blend mode, u16 materialFlags) {
    if (materialFlags & kM2Unlit)
        return false;
    return mode != M2Blend::Mod && mode != M2Blend::Mod2x;
}

Vector3f M2CombinerInput(M2Blend mode, const Vector3f& batchColor, const Vector3f& geosetColor) {
    if (mode == M2Blend::Mod)
        return {1.0f, 1.0f, 1.0f};
    if (mode == M2Blend::Mod2x)
        return {0.5f, 0.5f, 0.5f};
    return {batchColor.x * geosetColor.x, batchColor.y * geosetColor.y,
            batchColor.z * geosetColor.z};
}

M2DrawState M2StateFor(M2Blend mode, u16 materialFlags, f32 elementAlpha, bool mirrored) {
    M2DrawState s;
    s.blend = M2BlendDesc(mode);
    s.alphaRef = M2AlphaRef(mode, elementAlpha);

    // Always tested. SetupMaterial picks between two GxDSState presets on
    // kM2NoDepthWrite alone — {3,7} and {1,7}, differing in the write bit — and
    // never reads kM2NoDepthTest at all.
    s.depth.depthTest = true;
    s.depth.depthWrite = (materialFlags & kM2NoDepthWrite) == 0;
    s.depth.depthCompare = gfx::CompareOp::LessEqual;

    s.raster.cull = (materialFlags & kM2TwoSided) ? gfx::CullMode::None
                    : mirrored                    ? gfx::CullMode::Front
                                                  : gfx::CullMode::Back;
    s.raster.frontCCW = true;

    s.fog = (materialFlags & kM2Unfogged) ? M2FogMode::Disabled : M2FogModeFor(mode);
    s.lit = M2LightingEnabled(mode, materialFlags);
    return s;
}

} // namespace whiteout::flakes::renderer::profiles::wow
