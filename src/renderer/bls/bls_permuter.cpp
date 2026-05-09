#include "bls_permuter.h"

#include <initializer_list>

namespace whiteout::flakes::renderer::bls {

namespace {

u32 Pack(std::initializer_list<u32> radices,
              std::initializer_list<u32> dims) {
    const auto n = radices.size();
    u32 packed = 0;
    u32 stride = 1;
    auto r = radices.begin();
    auto d = dims.begin();
    for (usize i = 0; i < n; ++i) {
        packed += (*d++) * stride;
        stride *= (*r++);
    }
    return packed;
}

u32 WeightIndex(u8 numWeights) {
    switch (numWeights) {
        case 1: return 1;
        case 4: return 2;
        default: return 0;
    }
}

u32 FogIndex(const RenderState& s) {
    return s.fogEnabled ? (u32(s.fogStyle) + 1u) : 0u;
}

}

PermuteIndices SelectPermutes(const RenderState& s) {
    const u32 flags  = s.materialFlags;
    const u32 lights = (!s.prepass && s.lightingEnabled && s.numLights > 0) ? 1u : 0u;
    const u32 fogIdx = FogIndex(s);

    const u32 fogIdxPrepass = s.prepass ? 0u : fogIdx;
    const u32 depthWrite = s.depthWrite ? 1u : 0u;
    const u32 prepass    = s.prepass    ? 1u : 0u;
    const u32 shadows    = s.shadows    ? 1u : 0u;
    const u32 alphaTest  = (s.alphaMode != 0) ? 1u : 0u;
    const u32 teamColor  = s.teamColor ? 1u : 0u;
    const u32 debug      = s.debugShader ? 1u : 0u;

    PermuteIndices out{0, 0};

    switch (s.shaderId) {
        case GxShaderID::CornFx: {

            out.vs = Pack({2,2,2,3,3}, {
                (flags >> 5) & 1u,
                (flags >> 4) & 1u,
                lights,
                (flags >> 2) & 3u,
                flags        & 3u,
            });

            out.ps = Pack({2,4,2,2,2,2,3,3}, {
                depthWrite,
                fogIdx,
                (flags >> 6) & 1u,
                (flags >> 5) & 1u,
                (flags >> 4) & 1u,
                lights,
                (flags >> 2) & 3u,
                flags        & 3u,
            });
            break;
        }
        case GxShaderID::HD:
        case GxShaderID::Crystal: {

            out.vs = Pack({2,3,2,3,2,2}, {
                u32(s.numTangents != 0 ? 1 : 0),
                WeightIndex(s.numWeights),
                u32(s.numColors),
                u32(s.numTexCoords),
                prepass,
                shadows,
            });

            out.ps = Pack({2,2,2,2,4,2,2,2}, {
                depthWrite,
                prepass,
                shadows,
                lights,
                fogIdxPrepass,
                alphaTest,
                teamColor,
                debug,
            });
            break;
        }
        case GxShaderID::SD_on_HD: {
            out.vs = Pack({2,3,2,3,2,2}, {
                u32(s.numTangents != 0 ? 1 : 0),
                WeightIndex(s.numWeights),
                u32(s.numColors),
                u32(s.numTexCoords),
                prepass,
                shadows,
            });

            u32 alphaDim = (s.alphaMode == 2  ) ? 2u : alphaTest;
            out.ps = Pack({2,2,2,2,4,3,2}, {
                depthWrite,
                prepass,
                shadows,
                lights,
                fogIdxPrepass,
                alphaDim,
                debug,
            });
            break;
        }
        case GxShaderID::SD: {
            out.vs = Pack({3,2,3,9}, {
                WeightIndex(s.numWeights),
                u32(s.numColors),
                u32(s.numTexCoords),
                u32(s.numLights),
            });

            out.ps = Pack({4,2,5,5}, {
                fogIdx,
                alphaTest,
                1u,
                0u,
            });
            break;
        }
        case GxShaderID::Terrain: {
            out.vs = Pack({2,2,2}, {
                prepass,
                shadows,
                u32(s.numColors),
            });
            out.ps = Pack({2,2,2,2,4,2}, {
                depthWrite,
                prepass,
                shadows,
                s.darkerShadows ? 1u : 0u,
                fogIdxPrepass,
                debug,
            });
            break;
        }
        case GxShaderID::CliffBlightMiscTerrain: {
            out.vs = Pack({2,2}, {prepass, shadows});
            out.ps = Pack({2,2,2,2,4,2,2}, {
                depthWrite,
                prepass,
                shadows,
                s.darkerShadows ? 1u : 0u,
                fogIdxPrepass,
                alphaTest,
                debug,
            });
            break;
        }
        case GxShaderID::Foliage: {
            out.vs = Pack({2,2,2}, {
                prepass,
                shadows,
                flags & 1u,
            });
            out.ps = Pack({2,2,2,4,2,2}, {
                depthWrite,
                prepass,
                shadows,
                fogIdxPrepass,
                alphaTest,
                debug,
            });
            break;
        }
        case GxShaderID::Water:
        case GxShaderID::Fog:
        case GxShaderID::ConeIndicator: {
            out.vs = 0;
            out.ps = Pack({4}, {fogIdxPrepass});
            break;
        }
        case GxShaderID::Sprite: {
            out.vs = 0;
            out.ps = Pack({2,2}, {(flags >> 1) & 1u, flags & 1u});
            break;
        }
        case GxShaderID::Movie: {
            out.vs = 0;
            out.ps = Pack({2,2,3,2}, {
                (flags >> 3) & 1u,
                (flags >> 2) & 1u,
                (flags >> 1) & 1u,
                flags        & 1u,
            });
            break;
        }
        case GxShaderID::BloomCombine: {
            out.vs = 0;
            out.ps = Pack({2}, {s.clampBloomOutput ? 1u : 0u});
            break;
        }
        case GxShaderID::Imgui: {
            out.vs = 0;
            out.ps = Pack({2}, {flags & 1u});
            break;
        }
        default: {
            out.vs = 0;
            out.ps = 0;
            break;
        }
    }

    return out;
}

PermuteCounts ExpectedPermuteCounts(GxShaderID id) {
    switch (id) {
        case GxShaderID::CornFx:              return {  72, 2304 };
        case GxShaderID::HD:
        case GxShaderID::Crystal:                return { 144,  512 };
        case GxShaderID::SD_on_HD:               return { 144,  384 };
        case GxShaderID::SD:                     return { 162,  200 };
        case GxShaderID::Terrain:                return {   8,  128 };
        case GxShaderID::CliffBlightMiscTerrain: return {   4,  256 };
        case GxShaderID::Foliage:                return {   8,  192 };
        case GxShaderID::Water:
        case GxShaderID::Fog:
        case GxShaderID::ConeIndicator:          return {   1,    4 };
        case GxShaderID::Sprite:                 return {   1,    4 };
        case GxShaderID::Movie:                  return {   1,   24 };
        case GxShaderID::BloomCombine:           return {   1,    2 };
        case GxShaderID::Imgui:                  return {   1,    2 };
        default:                                 return {   1,    1 };
    }
}

}
