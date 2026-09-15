#pragma once

// ============================================================================
// d3::MaterialDesc — a Diablo III particle's material, which is not a
// Diablo III *surface's* material. `Particle_DrawBatch` @0x71000B63A0 binds four
// raw texture ids off the system record, one per STAGE TYPE (1, 19, 12, 14),
// combined through `Billboard.fx` / `vs_legacy` / `ps_legacy`, each layer with
// its own UV transform. See D3_PARTICLE_DESIGN.md §8, §8.1 and §30.2.
// ============================================================================

#include "io/d3/d3_types.h"
#include "types.h"
#include "whiteout/flakes/types.h"

#include <array>
#include <memory>
#include <string>

namespace whiteout::flakes::io {
struct D3TextureAtlas;
}

namespace whiteout::flakes::renderer::particle::d3 {

/// One `MaterialTextureEntry` of a `.prt`, resolved.
struct MaterialLayer {
    /// The `.tex` this layer names. -1 leaves the layer unbound, which draws
    /// as white rather than as nothing — the same answer an unresolved stage
    /// gives in the original.
    i32 textureSno = -1;
    /// Index into the owning actor's texture scope, assigned when the emitter
    /// is created. -1 until then, and for an emitter with no actor.
    i32 textureId = -1;
    /// The entry's `EMaterialTextureType`: 1, 19, 12 or 14. The bind order is
    /// this array's order; the type is what the stage chain keys a layer's
    /// texcoord set, wrap bits and combine on.
    i32 rawType = 0;
    /// From the entry's UV flags word, bits 0 and 1.
    u32 wrapFlags = 0x3;
    /// The entry's own transform, evaluated per frame against the system clock.
    ::whiteout::flakes::io::D3UvXform uv;

    /// @brief Which of the vertex's four TEXCOORDS this stage samples at: a
    ///        POSITIONAL uv set (0 type 1, 1 type 19, 2 type 12, 3 type 14), not
    ///        this array's index. An absent set bakes the identity rectangle (§30.2).
    u32 uvSet = 0;

    /// @brief The frame table on this layer's TEXTURE, or null where it has
    ///        none. Resolved for every layer: stage 0's sheet shapes the quad
    ///        whatever the entry's uv mode says (§27.1, §30.2).
    std::shared_ptr<const ::whiteout::flakes::io::D3TextureAtlas> atlas;
    /// @brief The flip-book's playback parameters, from `tAnim4` / `tAnim5` via
    ///        `Anim2D_BindAndInit`'s params block (runtime `entry+120` = file
    ///        `entry+0x84`); base and range are REINTERPRETED AS INTEGERS (§30.2).
    f32 atlasRate = 0.0f;      ///< Frames per second; 0 = a still frame.
    f32 atlasRateJitter = 0.0f; ///< Added, times a uniform draw, per particle.
    i32 atlasFrameBase = 0;
    i32 atlasFrameRange = 0; ///< The draw is `base + rand % (range + 1)`.
    bool atlasLoops = true;  ///< `.an2`+20: 2 loops, 0 plays once and stops.
    /// @brief This layer's step in the pass's combine chain, per channel: skip,
    ///        modulate or add. Modulate by default, for a pass naming no stages;
    ///        a layer of a type a naming pass never declares is skipped (§24, §30.2).
    u8 colorOp = ::whiteout::flakes::io::kD3StageModulate;
    u8 alphaOp = ::whiteout::flakes::io::kD3StageModulate;
    /// The stage's own MODULATE2X / MODULATE4X gain and its clamp. Per stage
    /// rather than accumulated, because a clamp in the middle of a chain is
    /// what separates `am2x_am2x` from `am4x` — 1,295 systems carry one.
    f32 colorGain = 1.0f;
    f32 alphaGain = 1.0f;
    bool colorClamp = false;
    bool alphaClamp = false;
};

/// The whole of a Diablo III particle's material.
struct MaterialDesc {
    /// Four, because four is what `Particle_DrawBatch` binds.
    static constexpr usize kMaxLayers = 4;

    std::array<MaterialLayer, kMaxLayers> layers{};
    /// How many of @ref layers are live, in bind order.
    u32 layerCount = 0;

    /// @brief Which entry of @ref layers fills each POSITIONAL uv set (types 1,
    ///        19, 12, 14), or -1 for a hole. The engine's slots keep holes; this
    ///        array is compacted, and 61 shipped materials differ (§30.2).
    std::array<i8, kMaxLayers> setLayer{{-1, -1, -1, -1}};

    /// @brief Did the ShaderMap chain resolve to a RenderPass?
    ///
    /// False leaves the defaults below standing, which are what the shipped
    /// particle passes overwhelmingly say: cull nothing, write no depth,
    /// blend SrcAlpha/One. Guessing that is better than guessing opaque.
    bool passResolved = false;

    /// @brief This emitter draws into the DISTORTION buffer, not the scene:
    ///        `RenderPass::dwUnknown00 == 3` (io/d3/d3_types.h). Only the target
    ///        changes, not the shading. Five shipped shaders; see §30.2.
    bool distortion = false;

    /// The ENGINE's blend enum, not D3DBLEND.
    enum EngineBlend : u32 { kBlendOne = 2, kBlendSrcAlpha = 5 };
    /// `D3DCMPFUNC`. Always reads as "no depth test at all" to the engine, and
    /// every premultiplied pass says it; the default is the ordinary test.
    enum DepthCompare : u32 { kCmpLessEqual = 4, kCmpAlways = 8 };

    bool blendEnable = true;
    u32 blendSrc = kBlendSrcAlpha;
    u32 blendDst = kBlendOne;
    bool depthWrite = false;
    u32 depthFunc = kCmpLessEqual;
    /// @brief The `_pma` output form: 0 none, 1 premultiply and invert the
    ///        alpha, 2 premultiply and write 1, 3 premultiply only.
    ///
    /// 3,175 of the corpus's 21,593 systems resolve to a pass that needs one.
    /// See D3PassState::pmaMode.
    u32 pmaMode = 0;
    /// The pass's two colour-write flags. 171 of the corpus's 243 billboard
    /// passes mask the alpha off. See D3PassState::colorWrite.
    bool colorWrite = true;
    bool alphaWrite = true;
    /// D3DRS_DEPTHBIAS, applied in the vertex shader. Twelve billboard passes
    /// carry one, all named `*_biased` / `*_zbias` / `*_biasPos`.
    /// See D3PassState::depthBias.
    f32 depthBias = 0.0f;
    /// `RenderPass+60 / 255`, and zero only when the pass switched the test off.
    f32 alphaTest = 0.0f;
    /// The comparison the reference goes through, D3DCMPFUNC, or 0 for no test.
    /// See D3Surface::alphaTestFunc for why the reference alone is not enough.
    u32 alphaFunc = 0;

    /// @brief Where the vertex colour enters each chain — see
    ///        D3PassState::colorVcolFirst.
    bool colorVcolFirst = true;
    bool colorVcolLast = false;
    bool alphaVcolFirst = true;
    bool alphaVcolLast = false;

    /// `szEffectFile` — `Billboard.fx` for every shipped particle. Carried so
    /// the corpus gate can say so rather than assume it.
    std::string effectFile;

    /// `szPixelShaderEntry`, kept for the census and the diagnostics dump.
    std::string pixelEntry;

    /// @brief Which of the three program shapes @ref pixelEntry names.
    ///
    /// The `.shd` says outright which pixel program the pass runs, and eleven
    /// of the corpus's twelve billboard entry points are NOT the fixed-function
    /// chain. 1,662 of 17,503 systems, and they were all drawn as chains.
    ::whiteout::flakes::io::D3ParticleProgram program =
        ::whiteout::flakes::io::D3ParticleProgram::Chain;

    /// @brief The first layer that is a FLOW MAP rather than a combine stage,
    ///        or -1. Every other layer's UV is offset by
    ///        `(product * 2^(n-1) - 0.5) * 0.5`; the first flow layer is fixed
    ///        by the entry point and the count by the pass's texcoords (§25).
    i32 flowFirst = -1;
    /// The last flow layer, inclusive. Meaningless when @ref flowFirst is -1.
    i32 flowLast = -1;

    /// @brief Does this program fade against the scene depth?
    ///
    /// `SoftBillboard.fx` multiplies every channel by
    /// `saturate((sceneDepth - viewDepth) / 2)`. Recorded but not applied: the
    /// scene target is not sampleable in SD. 1,911 systems.
    bool softFade = false;

    /// @brief The dissolve tail: `alpha = min(1, pow(alpha, 10 * COLOR1.a))`,
    ///        168 systems. See io/d3/d3_types.h::kD3StageAlphaErosion, and
    ///        §26 / §29.5 for what writes COLOR1.
    bool erosion = false;


    /// @brief The diffuse, by TYPE rather than by position:
    ///        `Particle_BindDrawTextures` @0x71000B7620 parks type 1 in its own
    ///        slot (G-D3P-23, §30.2). Falls back to the first layer, not white.
    i32 DiffuseTextureId() const {
        for (u32 i = 0; i < layerCount; ++i)
            if (layers[i].rawType == 1)
                return layers[i].textureId;
        return layerCount > 0 ? layers[0].textureId : -1;
    }
};

} // namespace whiteout::flakes::renderer::particle::d3
