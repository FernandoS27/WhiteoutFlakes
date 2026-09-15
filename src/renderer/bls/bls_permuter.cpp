#include "bls_permuter.h"

#include <algorithm>
#include <initializer_list>

namespace whiteout::flakes::renderer::bls {

namespace {

u32 Pack(std::initializer_list<u32> radices, std::initializer_list<u32> dims) {
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

// The engine's vertex-format table yields a weight count of 0 or 4; 1 has a
// slot of its own in the radix but compiles to the unskinned program.
u32 WeightIndex(u8 numWeights) {
    switch (numWeights) {
    case 1:
        return 1;
    case 4:
        return 2;
    default:
        return 0;
    }
}

u32 Bit(bool b, u32 mask) {
    return b ? mask : 0u;
}

// HD VS, mixed radix: boneBuffer + 2*tangent + 4*weightIndex + 12*color + 24*uv.
u32 HdVsIndex(const RenderState& s) {
    return Pack({2, 2, 3, 2, 3}, {
                                     s.boneBuffer && s.numWeights == 4 ? 1u : 0u,
                                     s.numTangents != 0 ? 1u : 0u,
                                     WeightIndex(s.numWeights),
                                     s.numColors != 0 ? 1u : 0u,
                                     std::min<u32>(s.numTexCoords, 2u),
                                 });
}

// The six low bits every 3.0.0 lit mesh family shares, in Crystal / SD_on_HD
// order (HD's mask is the same with AO_MAP inserted below them).
u32 LitMeshLowBits(const RenderState& s) {
    return Bit(s.shadowCascade2, 1) | Bit(s.mrt, 2) | Bit(s.depthPrepass, 4) |
           Bit(s.lightDebug, 8) | Bit(s.pointShadows, 16) | Bit(s.shadowCascade, 32);
}

} // namespace

GxShaderID ProgramForLayer(i32 mdxShaderType) {
    switch (static_cast<MdxShaderType>(mdxShaderType)) {
    case MdxShaderType::HdDefaultUnit:
        return GxShaderID::HD;
    case MdxShaderType::HdCrystal:
        return GxShaderID::Crystal;
    case MdxShaderType::SdLegacy:
    case MdxShaderType::SdFixedFunction:
    default:
        return GxShaderID::SD_on_HD;
    }
}

PermuteIndices SelectPermutes(const RenderState& s) {
    const u32 flags = s.materialFlags;
    const bool alphaTest = s.alphaMode != 0;

    PermuteIndices out{0, 0};

    switch (s.shaderId) {
    case GxShaderID::HD:
        out.vs = HdVsIndex(s);
        out.ps = Bit(s.aoMap, 1) | (LitMeshLowBits(s) << 1) | Bit(s.lighting, 128) |
                 Bit(alphaTest, 256) | Bit(s.multiLayer, 512);
        break;
    case GxShaderID::Crystal:
        // HD's mask with AO_MAP struck out.
        out.vs = HdVsIndex(s);
        out.ps = LitMeshLowBits(s) | Bit(s.lighting, 64) | Bit(alphaTest, 128) |
                 Bit(s.multiLayer, 256);
        break;
    case GxShaderID::SD_on_HD: {
        // Six feature bits under an outer `lit + 2 * sub` field, sub 0 plain,
        // 1 alpha test, 2 sRGB encode. SD-on-HD draws into the linear HDR scene
        // target, so the sRGB sub is only for a caller that asks for it.
        out.vs = HdVsIndex(s);
        const u32 sub = alphaTest ? 1u : (s.srgbOutput ? 2u : 0u);
        out.ps = LitMeshLowBits(s) + 64u * ((s.lighting ? 1u : 0u) + 2u * sub);
        break;
    }
    case GxShaderID::SD:
        out.vs = Pack({3, 2, 3, 9}, {
                                        WeightIndex(s.numWeights),
                                        s.numColors != 0 ? 1u : 0u,
                                        std::min<u32>(s.numTexCoords, 2u),
                                        std::min<u32>(s.numLights, 8u),
                                    });
        out.ps = Pack({7, 2, 5, 5}, {
                                        std::min<u32>(s.sdFogMode, 6u),
                                        alphaTest ? 1u : 0u,
                                        std::min<u32>(s.sdStage0, 4u),
                                        std::min<u32>(s.sdStage1, 4u),
                                    });
        break;
    case GxShaderID::Terrain:
        out.vs = s.numColors != 0 ? 1u : 0u;
        out.ps = LitMeshLowBits(s);
        break;
    case GxShaderID::Foliage:
        out.vs = flags & 1u;
        out.ps = LitMeshLowBits(s) | Bit(alphaTest, 64);
        break;
    case GxShaderID::CliffBlightMiscTerrain:
        out.vs = 0;
        out.ps = LitMeshLowBits(s) | Bit(alphaTest, 128);
        break;
    case GxShaderID::Water:
        out.vs = 0;
        out.ps = Bit(s.shadowCascade2, 1) | Bit(s.depthPrepass, 2) | Bit(s.lightDebug, 4) |
                 Bit(s.pointShadows, 8) | Bit(s.shadowCascade, 16);
        break;
    case GxShaderID::Fog:
        out.vs = 0;
        out.ps = std::min<u32>(s.sdFogMode, 6u);
        break;
    case GxShaderID::Sprite:
        out.vs = 0;
        out.ps = Pack({2, 2}, {(flags >> 1) & 1u, flags & 1u});
        break;
    case GxShaderID::Movie:
        out.vs = 0;
        out.ps = std::min<u32>(flags, 23u);
        break;
    case GxShaderID::BloomCombine:
        out.vs = 0;
        out.ps = s.clampBloomOutput ? 1u : 0u;
        break;
    case GxShaderID::Imgui:
        out.vs = 0;
        out.ps = flags & 1u;
        break;
    default:
        break;
    }

    return out;
}

// Counts from externals/Wc3Shaders/wc3_shaders.json, which match the retail
// 3.0.0 bundles (u32 at offset 12 of each .bls). The VS count of a program
// that borrows another family's VS is that family's count.
PermuteCounts ExpectedPermuteCounts(GxShaderID id) {
    switch (id) {
    case GxShaderID::HD:
        return {72, 1024};
    case GxShaderID::Crystal:
        return {72, 512};
    case GxShaderID::SD_on_HD:
        return {72, 384};
    case GxShaderID::SD:
        return {162, 350};
    case GxShaderID::CornFx:
        return {72, 288};
    case GxShaderID::Terrain:
        return {2, 128};
    case GxShaderID::Foliage:
        return {2, 128};
    case GxShaderID::CliffBlightMiscTerrain:
        return {1, 256};
    case GxShaderID::Water:
        return {1, 128};
    case GxShaderID::Fog:
        return {1, 7};
    case GxShaderID::Sprite:
        return {1, 4};
    case GxShaderID::Movie:
        return {1, 24};
    case GxShaderID::BloomCombine:
        return {1, 2};
    case GxShaderID::Imgui:
        return {1, 2};
    default:
        return {1, 1};
    }
}

} // namespace whiteout::flakes::renderer::bls
