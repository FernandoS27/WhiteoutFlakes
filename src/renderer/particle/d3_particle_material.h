#pragma once

// ============================================================================
// d3::MaterialDesc — a Diablo III particle's material, which is not a
// Diablo III *surface's* material.
//
// `Particle_DrawBatch` @0x71000B63A0 resolves the `.prt`'s own `snoShaderMap`
// through the same ShaderMap path geometry uses and then stops: no UberMaterial
// resolve, no Material SNO, no `Render_ResolveMaterialTextureStages`, no
// translucent/opaque pair, and the pass index is the literal 0. What it binds
// instead is four raw texture ids straight off the system record, one per
// STAGE TYPE:
//
//     type  1 <- sys+300      type 19 <- sys+304
//     type 12 <- sys+308      type 14 <- sys+312
//
// and those four are exactly the types the `.prt`'s own `UberMaterial` carries.
// Measured over 21,593 shipped files / 35,631 entries:
//
//     type   1  18,462     type 19  13,157     type 12  2,066     type 14  1,622
//     sets:  {1,19} 10,904   {1} 5,311   none 3,120   {1,12,14,19} 1,431
//
// The other twelve types that appear (25, 16, 40, 42, 58 …, 300 entries
// between them) are declared by no particle pass and are not bound.
//
// The chain that combines them is `Billboard.fx` / `vs_legacy` / `ps_legacy` —
// the engine's FIXED-FUNCTION family — so the per-stage combine block
// io/d3/d3_types.h already decodes for `Legacy.fx` is the authority here too.
// `particle_additive` is one modulate stage; `Particle_transparent_am4x_errosion`
// is three, with `24 24` on the alpha group, which is where the `am4x` in its
// name comes from.
//
// Every layer carries its OWN UV transform, and for particles that is not a
// detail: 9,600 mode-2 entries scale their coordinates (3,372 of them by
// exactly 0.5, 0.5 — a quarter-tile of a sprite sheet) and 13,996 scroll. A
// build that samples all four layers at the quad's own UV draws the whole
// sheet on every particle.
// ============================================================================

#include "io/d3/d3_sno_cache.h"
#include "io/d3/d3_types.h"
#include "types.h"
#include "whiteout/flakes/types.h"

#include <array>
#include <memory>
#include <string>

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
    /// The entry's `EMaterialTextureType`: 1, 19, 12 or 14. Diagnostics only —
    /// the bind order is this array's order.
    i32 rawType = 0;
    /// From the entry's UV flags word, bits 0 and 1.
    u32 wrapFlags = 0x3;
    /// The entry's own transform, evaluated per frame against the system clock.
    ::whiteout::flakes::io::D3UvXform uv;

    /// @brief Which of the vertex's four TEXCOORDS this stage samples at.
    ///
    /// `TAG_VS_TEXCOORD{i}_FUNC` names a uv SET, and the set is positional over
    /// the stage types (0 type 1, 1 type 19, 2 type 12, 3 type 14) -- not this
    /// array's compacted index. It is usually the stage's own set and sometimes
    /// another's: 1,211 systems route one slot onto a neighbour's coordinate.
    /// An absent set carries the identity rectangle, which is what
    /// `Particle_WriteQuadVertices` bakes for a stage with no state.
    u32 uvSet = 0;

    /// @brief The frame table on this layer's TEXTURE, or null where it has
    ///        none. Resolved for every layer, not only the uv mode 3 ones.
    ///
    /// The sheet is a property of the `.tex`, and the entry's uv mode decides
    /// only what is done with it. `Particle_WriteQuadVertices` takes the quad's
    /// base rectangle and its vertical aspect off **stage 0's** sheet before it
    /// has read the mode at all, so a type-1 layer carrying frames shapes the
    /// quad whatever its entry says; mode 3 on top of that walks the origin.
    std::shared_ptr<const ::whiteout::flakes::io::D3TextureAtlas> atlas;
    /// @brief The flip-book's playback parameters, read out of `tAnim4` and
    ///        `tAnim5` — the two anim triples the UV transform never uses.
    ///
    /// `MatTex_InitUvState` hands `entry+120` (runtime) straight to
    /// `Anim2D_BindAndInit` as its params block, which reads +4, +8, +12 and
    /// +16 from it and nothing else. Runtime offsets are the file's MINUS 12,
    /// so the block starts at file `entry+0x84` and the four fields are
    /// `tAnim4.flRate1`, `tAnim5.flAmount`, `tAnim5.flRate0` and
    /// `tAnim5.flRate1` — the last two REINTERPRETED AS INTEGERS, which is what
    /// makes them read as 0, 3, 7, 15, 23, 63 rather than as denormals.
    ///
    /// The block's own first word (`tAnim4.flRate0`) is the `.an2` handle, which
    /// `Anim2D_BindAndInit` never touches — it binds the TEXTURE's frame table —
    /// and which `Anim2D_AdvanceCursor` consults for one thing, the loop mode at
    /// the asset's +20. It is **0 on 6,934 mode-3 entries and 2 on 1,456**, and
    /// 0 is a live SNO id rather than "absent" (only -1 stops the advance), so
    /// every one of the 8,390 has a running flip-book. What is not known here is
    /// what asset 0 says: @ref atlasLoops assumes 2/loop for both.
    f32 atlasRate = 0.0f;      ///< Frames per second; 0 = a still frame.
    f32 atlasRateJitter = 0.0f; ///< Added, times a uniform draw, per particle.
    i32 atlasFrameBase = 0;
    i32 atlasFrameRange = 0; ///< The draw is `base + rand % (range + 1)`.
    bool atlasLoops = true;  ///< `.an2`+20: 2 loops, 0 plays once and stops.
    /// @brief This layer's step in the pass's combine chain, per channel.
    ///
    /// `kD3StageSkip` leaves the channel alone, `kD3StageModulate` multiplies
    /// the texture in, `kD3StageAdd` adds it. Both default to Modulate, because
    /// a pass that names no stage is saying "the program decides" and every
    /// particle program that binds a layer samples it — but a pass that DOES
    /// name its stages is the authority, and a layer of a type the pass never
    /// declares is skipped outright: 3,242 of the corpus's 17,503 systems carry
    /// a texture their own pass has no stage for, and the shipped program never
    /// samples it. Type 19 is the common case, alpha-only on 55 of the corpus's
    /// 243 billboard passes and both ways on 95 — an alpha mask against a second
    /// colour layer.
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

    /// The `.shm` the pass came from, or -1. Kept for the census.
    i32 snoShaderMap = -1;

    /// @brief Which entry of @ref layers fills each POSITIONAL uv set (types 1,
    ///        19, 12, 14), or -1 for a hole.
    ///
    /// The engine's stage slots are positional with holes -- `Particle_BindDrawTextures`
    /// parks type 1 in slot 0 and type 14 in slot 3 whatever else is present --
    /// while this array is compacted to the types the material carries. The two
    /// only agree when the types are a prefix of the bind order, and 61 shipped
    /// materials are not.
    std::array<i8, kMaxLayers> setLayer{{-1, -1, -1, -1}};

    /// @brief Did the ShaderMap chain resolve to a RenderPass?
    ///
    /// False leaves the defaults below standing, which are what the shipped
    /// particle passes overwhelmingly say: cull nothing, write no depth,
    /// blend SrcAlpha/One. Guessing that is better than guessing opaque.
    bool passResolved = false;

    /// @brief This emitter draws into the DISTORTION buffer, not the scene.
    ///
    /// `RenderPass::dwUnknown00 == 3` — see io/d3/d3_types.h. Five shipped
    /// billboard shaders carry it (`particle_distortion`,
    /// `particle_distortion_cm2x_am1x`, `particle_distortion_cm2x_am2x`,
    /// `particle_fullscreen_distortion`, `particle_fullscreen_distortion_am4x`)
    /// and every one of them is `Billboard.fx::ps_legacy`, so nothing about the
    /// shading changes — only the target. Drawn in the scene instead, what they
    /// paint is their distortion map as if it were colour: a hard blue-green
    /// rectangle over the model, which is what the Mystic Ally's caustics were.
    bool distortion = false;

    bool blendEnable = true;
    u32 blendSrc = 5; ///< The ENGINE's blend enum, not D3DBLEND. 5 = SrcAlpha.
    u32 blendDst = 2; ///< 2 = One.
    bool depthWrite = false;
    /// The pass's depth compare, D3DCMPFUNC. 8 = Always, which the engine reads
    /// as "no depth test at all" — and every premultiplied pass says 8.
    u32 depthFunc = 4;
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
    ///        or -1 where the program has none.
    ///
    /// A flow shader samples one to three maps, multiplies them, recentres the
    /// pair on zero and offsets every OTHER layer's UV by it:
    ///
    ///     offset = (product * 2^(n-1) - 0.5) * 0.5
    ///
    /// which is one expression for all three counts. Which layers those are is
    /// fixed by the entry point (`ps_particle_flow` warps from layer 1 on,
    /// `ps_legacy_flow` from 2, `ps_firewall_flow` from 3), and how many of them
    /// are live is the pass's own texcoord count.
    i32 flowFirst = -1;
    /// The last flow layer, inclusive. Meaningless when @ref flowFirst is -1.
    i32 flowLast = -1;

    /// @brief Does this program fade against the scene depth?
    ///
    /// `SoftBillboard.fx` multiplies every channel by
    /// `saturate((sceneDepth - viewDepth) / 2)`. Recorded but not applied: the
    /// scene target is not sampleable in SD. 1,911 systems.
    bool softFade = false;

    /// @brief The dissolve tail: `alpha = min(1, pow(alpha, 10 * COLOR1.a))`.
    ///
    /// COLOR1 is a per-particle constant written ONCE at emit
    /// (`ParticleSystem_EmitParticle` copies it out of the emit context and
    /// nothing else ever writes it), and with no emit context it is
    /// 0xFFFFFFFF -- so `.a` is 1 and the exponent is a flat 10. What eats the
    /// sprite away over its life is the alpha the chain hands it, not this.
    /// 168 systems. See io/d3/d3_types.h::kD3StageAlphaErosion.
    bool erosion = false;


    /// @brief The diffuse, by TYPE rather than by position.
    ///
    /// `Particle_BindDrawTextures` @0x71000B7620 asks the material for types
    /// 1, 19, 12 and 14 in that order and parks each in its own slot, so the
    /// diffuse is whichever entry says 1 and never "the first one" (G-D3P-23).
    /// The two agree on 18,412 of the corpus's 18,473 textured materials and
    /// disagree on 61, which used to bind an alpha mask as the diffuse.
    /// Falling back to the first layer keeps a material that names no type 1
    /// drawing something rather than white.
    i32 DiffuseTextureId() const {
        for (u32 i = 0; i < layerCount; ++i)
            if (layers[i].rawType == 1)
                return layers[i].textureId;
        return layerCount > 0 ? layers[0].textureId : -1;
    }
};

} // namespace whiteout::flakes::renderer::particle::d3
