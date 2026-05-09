#include <cornflakes/render/shader_perm.hpp>

namespace whiteout::cornflakes {

namespace {

namespace vs_inner {
inline constexpr u32 kHasRandom = 0x01U;
inline constexpr u32 kHasVC = 0x02U;
inline constexpr u32 kHasNT = 0x04U;
} // namespace vs_inner

namespace ps_inner {
inline constexpr u32 kWriteGBuffer = 0x01U;
inline constexpr u32 kFogLinear = 0x02U;
inline constexpr u32 kFogExp = 0x04U;
inline constexpr u32 kSoftParticles = 0x08U;
inline constexpr u32 kAlphaLut = 0x10U;
inline constexpr u32 kVertexColor = 0x20U;
inline constexpr u32 kLit = 0x40U;
} // namespace ps_inner

struct OuterParts {
    u32 modeIdx;
    u32 uvVariant;
};

OuterParts resolveOuter(const LayerRendererFlags& flags, RenderPass pass) noexcept {
    u32 modeIdx = 0;
    if (flags.isAtlas) {
        modeIdx = 2;
    } else if (flags.isBillboard) {
        modeIdx = 1;
    }

    u32 uvVariant = 0;
    if (pass == RenderPass::Motion) {

        uvVariant = 2;
    } else if (flags.hasUV) {
        uvVariant = 1;
    }

    return {modeIdx, uvVariant};
}

u32 computeVsInner(const LayerRendererFlags& flags, u32 uvVariant) noexcept {
    u32 inner = 0;

    if (flags.hasRandom && uvVariant != 0U) {
        inner |= vs_inner::kHasRandom;
    }
    if (flags.hasVC) {
        inner |= vs_inner::kHasVC;
    }
    if (flags.hasNT) {
        inner |= vs_inner::kHasNT;
    }
    return inner;
}

u32 fogBits(FogMode fog) noexcept {
    switch (fog) {
    case FogMode::Linear:
        return ps_inner::kFogLinear;
    case FogMode::Exp:
        return ps_inner::kFogExp;
    case FogMode::ExpSq:

        return ps_inner::kFogLinear | ps_inner::kFogExp;
    case FogMode::None:
    default:
        return 0;
    }
}

u32 computePsInner(const LayerRendererFlags& flags, FogMode fog, RenderPass pass) noexcept {
    u32 inner = 0;

    if (flags.hasSoftParticles) {
        inner |= ps_inner::kSoftParticles;
    }
    if (pass == RenderPass::Motion) {

        return inner;
    }
    if (flags.writeGBuffer) {
        inner |= ps_inner::kWriteGBuffer;
    }
    inner |= fogBits(fog);
    if (flags.hasAlphaLut && flags.hasUV) {

        inner |= ps_inner::kAlphaLut;
    }
    if (flags.hasVC) {
        inner |= ps_inner::kVertexColor;
    }
    if (flags.isLit && flags.hasNT) {
        inner |= ps_inner::kLit;
    }
    return inner;
}

} // namespace

ShaderPermKey classifyCornFxPerm(const LayerRendererFlags& flags, FogMode fog,
                                  RenderPass pass) noexcept {
    const auto outer = resolveOuter(flags, pass);
    const u32 outerKey = outer.modeIdx * 3U + outer.uvVariant;

    const u32 vsInner = computeVsInner(flags, outer.uvVariant);
    const u32 psInner = computePsInner(flags, fog, pass);

    ShaderPermKey key;
    key.vsPerm = outerKey * 8U + vsInner;
    key.psPerm = outerKey * 128U + psInner;
    key.pass = pass;
    return key;
}

} // namespace whiteout::cornflakes
