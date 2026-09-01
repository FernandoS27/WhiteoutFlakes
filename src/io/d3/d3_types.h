#pragma once

// Small value types shared by the Diablo III adapter and the diablo3 profile.
//
// Split out so `d3_surface_table.h` can take the adapter's emission order and
// canonical texture list without including the adapter (and the content
// provider behind it), and so neither side can drift a field.
//
// ---------------------------------------------------------------------------
// THREE FIELDS OF MaterialTextureEntry ARE NAMED FOR THE WRONG THING
//
// WhiteoutLib's 160-byte on-disk record reads
//
//     0x00 dwSlotIndex   0x0C dwTextureFlags   0x50.. tAnimU/V/Rotate   0x98 dwTextureType
//
// and every one of those three names describes a different field's job. What
// the engine actually does with the record (`Render_ResolveMaterialTextureStages`
// 0x71001DD980, `MatTex_BuildUvMatrix4x4`, `sub_71001D49D0`):
//
//     0x00  EMaterialTextureType   the LUT key. The resolve pass builds
//                                  `dest[entry->type] = entry` over a 62-slot
//                                  array and the bound RenderPass asks for
//                                  stages BY TYPE. `sub_71001D49D0(entries,
//                                  count, type)` is the by-type lookup, and
//                                  ActorModel_BuildDrawBatches calls it with
//                                  the literals 1, 12 and 14 for the water
//                                  surface — the same types a wing material
//                                  carries.
//     0x0C  UV TRANSFORM MODE      `MatTex_BuildUvMatrix4x4` switches on it, 0..6.
//     0x10  the 144-byte UV block  mode 1 reads a verbatim 4x4 from here;
//                                  mode 2 reads uScale at +0 and vScale at +20.
//     0x98  UV FLAGS               a bitfield; bit 3 rotates about (0.5, 0.5).
//
// Measured over 19,200 shipped material variants / 200,240 texture entries:
// **the field at 0x00 never repeats a value inside one material (0 of 19,200)**,
// which is the property a `dest[type] = entry` key must have, while the field at
// 0x98 repeats in 80.39% — it cannot be a key at all. Of 3,586 variants that
// carry exactly one texture entry, **3,585 give it type 1**: a material with one
// texture has a base map and nothing else, which is what makes type 1 the
// diffuse. The 0x98 histogram is {0, 1, 2, 3, 4, 5, 8, 9, 11} with bits 0 and 1
// carrying it — a flags word, not a 62-valued enum.
//
// Reading 0x98 as the type is what bound an arbitrary layer as the diffuse on
// every multi-layer material, and left the 1.67% of variants with no 0x98 == 0
// entry (Malthael's wings among them) with no texture at all.
//
// The three fields keep WhiteoutLib's names — it is a submodule and renaming
// them there is a separate change — so nothing outside this header touches them
// raw. Go through the accessors.
//
// ---------------------------------------------------------------------------
// THE PROGRAMS ARE READABLE AFTER ALL
//
// `d3_standard.slang` opens by saying the original's programs cannot be read.
// That was true of the console build's `pscod`/`vscod` blobs; it is not true of
// the game as shipped. `OpenGLShaders/` carries 7,236 ARB assemblies compiled
// by cgc from the same `.fx` sources, in plain text, with their sampler names
// in a string table — and a `RenderPass` names the program that consumes it
// (`szEffectFile`, `szVertexShaderEntry`, `szPixelShaderEntry`) beside an
// ORDERED list of the stage types it binds. So a material's semantics can be
// read off the shipped data end to end, and D3SlotOfType below is transcribed
// from it rather than inferred.
//
// Two consequences that are not local to the slot map:
//
//  * **The vertex colour is a LIGHT term, not a tint.** `vs_scene` ends
//    `MAD R0.xyz, vertex.attrib[3], 2, R0` / `MUL result.color.xyz, R0, 0.5` —
//    the attribute is ADDED into the light sum — and `vs_irrad_*` never reads
//    its RGB at all, only `.w`. Which is why 84.6% of shipped sub-objects carry
//    vertex colour (0,0,0) and 14.9% carry (255,255,255): the static
//    families bake level light there and the actor families do not use it. A
//    renderer that multiplies it into the albedo blacks out most of the
//    game's meshes. See D3PassState for the per-family split.
//
//  * **A material may carry types its pass never asks for.** The pass's stage
//    list is the authority on which entries are live. Measured over 2,208
//    resolved passes: types 26..38 are declared by ZERO of them (274/274 of
//    those entries undeclared), which is what the "model-wide 25..38 block" was
//    — data no pass binds.

#include "whiteout/flakes/types.h"

#include <whiteout/sno/d3/native/d3_native.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <optional>
#include <span>
#include <vector>

namespace whiteout::flakes::io {

namespace d3n = ::whiteout::sno::d3::native;

/// @brief One referenced texture, in canonical (first-seen, deduped) order.
struct D3TextureRef {
    i32 snoId = -1;
    /// @brief Wanted as a cubemap. Always false in v1: no slot the shading
    ///        model consumes samples a cube, and D3 states cube-ness inside the
    ///        `.tex` rather than on the material — so asking here would mean
    ///        reading every texture once to learn something nothing uses.
    bool cube = false;
};

/// @brief Bit 0 of `SubObjectAppearance::dwUnknown00`: does this sub-object
///        draw under the look that selected this variant?
///
/// `ActorModel_BuildSubObjectRenderRecords` (0x71002227A0) opens each record
/// with `record[0] = subObjectAppearance[0] & 1` and then mirrors bit 0 into
/// bit 1; nothing else in the record's construction reads the field. The
/// shipped data agrees exactly: Tyrael's two looks are `A` and `A_restored`,
/// and the bit is set on `A_normal_mat` (the Stranger) under the first, on
/// `A_restored_mat` + `A_restored_cloth` under the second, and on
/// `A_skeleton_mat` under neither — a death body is gameplay, not a look.
/// Imperius's `A_unarmed_wingless` clears it on `wing_mat` alone, and his
/// `Invisible` look clears it on all four of his materials.
///
/// Bit 2 is set on every shipped variant and bits 7 and 8 appear per
/// sub-object; none of the three has a recovered meaning, so none is acted on.
inline constexpr i32 kD3SubObjectVisibleBit = 0x1;

/// @brief The entry's `EMaterialTextureType` — the field at 0x00.
inline i32 D3TextureTypeOf(const d3n::MaterialTextureEntry& e) {
    return e.dwSlotIndex;
}

/// @brief The entry's UV transform mode — the field at 0x0C. See D3UvMode.
inline i32 D3UvModeOf(const d3n::MaterialTextureEntry& e) {
    return e.dwTextureFlags;
}

/// @brief The entry's UV flags word — the field at 0x98.
inline i32 D3UvFlagsOf(const d3n::MaterialTextureEntry& e) {
    return e.dwTextureType;
}

/// @brief `MatTex_BuildUvMatrix4x4`'s switch, verbatim.
///
/// Corpus population: 0 x 9,052, 1 x 32,139, 2 x 159,000, 3 x 35, 5 x 14 —
/// which is the switch's own case list and nothing outside it, so the field is
/// this enum and not a bitfield.
enum class D3UvMode : i32 {
    Identity = 0,
    /// A verbatim 4x4 at 0x10.
    Matrix = 1,
    /// uScale at 0x10, vScale at 0x24, then rotation and scroll from the
    /// per-frame animation state. 82.7% of shipped entries.
    ScaleRotateScroll = 2,
    /// An `Anim2D` frame table picks the sub-rect. Not reproduced.
    Anim2D = 3,
    /// An ENVIRONMENT-MAP projection, recovered by G-D3P-16: the render
    /// system's 4x4 at +1672 with elements 12..14 zeroed, inverted, then its
    /// first three columns negated. Not reproduced — the shading path has no
    /// view matrix to hand — but no longer unknown.
    Screen = 4,
    /// A bone transform drives the coordinates. Not reproduced.
    Bone = 5,
    BoneAlt = 6,
};

/// @brief Bit 3 of the UV flags word: pivot the rotation on the scroll offset
///        re-centred on 0.5, rather than rotating about the origin.
///
/// Measured form (G-D3P-16): `t = (scroll - 0.5) . M + 0.5`. The pivot is the
/// SCROLL, not a fixed (0.5, 0.5) — a distinction only a non-zero scroll can
/// see, and ten of the gate's cases do.
///
/// The one bit `MatTex_BuildUvMatrix4x4` tests (`*(_BYTE *)(a2 + 140) & 8`). Set on 26
/// entries in the corpus.
inline constexpr i32 kD3UvFlagRotateAboutCentre = 0x8;

/// @brief Bits 0 and 1 of the same word: RANDOMISE the initial U / V phase.
///
/// NOT an address mode, which is what they were read as until the particle
/// pass measured them. `MatTex_InitUvState` — the per-instance initialiser for one
/// texture stage's UV animation state — tests exactly these two bits and, for
/// uv mode 2 alone, seeds the scroll phase from `Rand_MWC_Next` instead of from
/// `flAmount`:
///
///     bit 0 set -> u0 = rand01()   clear -> u0 = tAnimU.flAmount
///     bit 1 set -> v0 = rand01()   clear -> v0 = tAnimV.flAmount
///
/// which is why the corpus distribution looked so much like an address mode:
/// 308,355 of the 325,227 zero entries are uv mode 0 or 1, where there is no
/// phase to randomise, and 74% of the 14,930 single-bit entries scroll in U
/// alone. Two readings, one histogram — the decompile is what separates them.
///
/// The real address mode is a property of the PASS, not of the entry: see
/// @ref D3StageWrapBits.
inline constexpr i32 kD3UvFlagRandomPhaseU = 0x1;
inline constexpr i32 kD3UvFlagRandomPhaseV = 0x2;

/// @brief One stage's U/V address modes as our own wrap bits (bit 0 = repeat U,
///        bit 1 = repeat V).
///
/// `TextureStageParams` +0x04 and +0x08, whose values are 1 and 2 and which are
/// **1 = clamp, 2 = wrap**. Three shipped shaders name their own answer, which
/// is as close to a labelled sample as this format offers:
///
///     Particle_transparent_am4x_errosion        (1,2,2) (19,2,2) (12,2,2)
///     Particle_transparent_am4x_clamp_errosion  (1,2,2) (19,2,1) (12,2,1)
///     softparticle_additive_am4x                (39,1,1) (1,1,1) (19,2,2)
///     softparticle_additive_am4x_wrap           (39,1,1) (1,2,2) (19,2,2)
///     trail_additive_diffAdd_am4x_clamp         (1,2,2)  (19,1,1)
///
/// and the families agree: `PostFX.fx` is 55/55 clamp and `TextureFilter.fx`
/// 58/59, while stage type 39 — the scene-colour copy — is clamp on 56 of 61.
inline u32 D3StageWrapBits(const d3n::TextureStageParams& stage) {
    return ((stage.dwAddressU == 2) ? 0x1u : 0u) | ((stage.dwAddressV == 2) ? 0x2u : 0u);
}

/// @brief Does an entry of this type name a texture THIS material owns?
///
/// The default branch of Render_ResolveMaterialTextureStages, plus the three
/// families that prefer the entry's own texture and only fall back to a core
/// asset when it is unset (2 -> default_lightmap, 3/47..52 -> flat_NM, 8 ->
/// irradiance). The types that IGNORE the entry are the engine render targets
/// (7, 9, 39, 60, 61), the per-draw computed ones (20, 22, 23, 53), the fixed
/// core assets (21, 24, 56, 59) and the three the pass skips outright (25, 40,
/// 41). Type 0 is an unused stage — `Render_ApplyPassRenderState` skips a stage
/// whose type id is 0.
///
/// Lives here rather than in the surface table because the adapter's canonical
/// texture list and the table's slot resolution have to agree on it *by
/// construction*: the list is an index space the table holds ids into.
inline bool D3TypeOwnsTexture(i32 type) {
    switch (type) {
    case 0:
    case 7:
    case 9:
    case 20:
    case 21:
    case 22:
    case 23:
    case 24:
    case 25:
    case 39:
    case 40:
    case 41:
    case 53:
    case 56:
    case 59:
    case 60:
    case 61:
        return false;
    default:
        return true;
    }
}

/// @brief The slots the shading model consumes, in shader order.
///
/// Here rather than beside the surface table because the adapter's canonical
/// texture list is exactly "the textures some slot samples", so the list and
/// the slot map have to be one statement or they drift. A slot's ordinal is the
/// register its texture binds to; appending only.
enum class D3SlotKind : u32 {
    Diffuse = 0,
    /// The type-3 family. Its unresolved default is `flat_NM` — the *identity*
    /// normal, not black. Confusing the two makes every un-normal-mapped
    /// surface face the viewer.
    Normal,
    Specular,
    Emissive,
    Lightmap,
    Irradiance,
    /// The alpha-mask layers, whose ALPHA multiplies into the surface's own.
    /// Three because a shipped material carries up to three of them (401
    /// variants carry one, 67 two, 18 three); the product is commutative, so
    /// which of 12/14/19 lands in which slot does not matter.
    AlphaMask0,
    AlphaMask1,
    AlphaMask2,
    Count,
};
inline constexpr u32 kD3SlotCount = static_cast<u32>(D3SlotKind::Count);

/// @brief Stages in a `Legacy.fx` fixed-function chain — the OTHER shape a D3
///        material resolves to, and the one the slots above cannot express.
///
/// Six, because the combine tag block is six wide and the reconstructed
/// `ps_legacy` samples `tex0..tex5`. Here rather than beside the shading model
/// because @ref D3UvTransformIdForStage keys the texture-animation palette on
/// it, and the adapter that fills that palette is on this side of the line.
inline constexpr u32 kD3MaxChainStages = 6;

/// @brief Which slot binds an entry of this `EMaterialTextureType`.
///
/// Named from the shipped programs, not guessed. A `RenderPass` carries
/// `szEffectFile` + entry names and an ORDERED stage list whose ids are these
/// types (`TextureStageParams` +0), and the corpus ships the OpenGL builds of
/// those programs beside the assets — 7,236 ARB assemblies under
/// `OpenGLShaders/`, each with its sampler names in texture-unit order. Pairing
/// a pass's stage list against its program's sampler list gives the type its
/// name directly:
///
///     1 diffuseSampler 268/268   2 lightMapSampler 258/258
///     4 environmentMapSampler 88/88   5 glossMapSampler 87/87
///     6 glowSampler   12/14/19 alphaMap0/1/2Sampler   20 waterSampler
///     22 shadowMapSampler 257/257   23 vignetteSampler 257/257
///     53 fogSampler   55 scumSampler   61 ssaoSampler 20/20
///
/// over 285 alignments anchored on the types the fallback trick had already
/// named. Type 8 stays Irradiance from that older evidence: the ActorIrrad.fx
/// programs ship no sampler names.
///
/// 4 (environment) and 11 (a second diffuse / overlay) have no slot here: this
/// shading model has no cube reflection term and no second base map, and giving
/// them one would be inventing the term as well as the binding.
inline D3SlotKind D3SlotOfType(i32 type) {
    switch (type) {
    case 1:
        return D3SlotKind::Diffuse;
    case 2:
        return D3SlotKind::Lightmap;
    case 3:
    case 47:
    case 48:
    case 49:
    case 50:
    case 51:
    case 52:
        return D3SlotKind::Normal;
    case 5:
        return D3SlotKind::Specular;
    case 6:
        return D3SlotKind::Emissive;
    case 8:
        return D3SlotKind::Irradiance;
    case 12:
        return D3SlotKind::AlphaMask0;
    case 14:
        return D3SlotKind::AlphaMask1;
    case 19:
        return D3SlotKind::AlphaMask2;
    default:
        return D3SlotKind::Count;
    }
}

/// @brief Is @p slot one of the alpha-mask layers?
inline bool D3SlotIsAlphaMask(D3SlotKind slot) {
    return slot == D3SlotKind::AlphaMask0 || slot == D3SlotKind::AlphaMask1 ||
           slot == D3SlotKind::AlphaMask2;
}

/// @brief One past the largest `EMaterialTextureType` the resolve pass indexes.
///
/// `Render_ResolveMaterialTextureStages` writes into a 62-slot array, so a type
/// is always a bit position in a u64 — which is how a pass's declared stage set
/// is carried.
inline constexpr i32 kD3TextureTypeCount = 62;

inline u64 D3TypeBit(i32 type) {
    return (type >= 0 && type < kD3TextureTypeCount) ? (1ull << type) : 0ull;
}

/// @brief One texture stage's combine code, from the fixed-function tag block.
///
/// `Legacy.fx` is the engine's fixed-function path and the only family that
/// carries this block: over 1,831 corpus passes it is present on **all 855**
/// `Legacy.fx` passes and on 320 of the 976 others, none of which is a family
/// this shading model reproduces. Three groups of six, one entry per stage:
///
///     0xA0010+i   the stage's op
///     0xA0016+i   its COLOUR combine    <- read here
///     0xA001C+i   its ALPHA combine     <- read here
///
/// The two combine codes are what say which channel a stage feeds, and the
/// shipped ARB programs are the oracle for reading them. `actor_glowTendril_
/// cm2x_bloom_skin` — Imperius's wings, stages (6, 1, 12, 14) — is the whole
/// grammar in one pass:
///
///     colour codes  20 20  0 24   ->  glow.rgb * diffuse.rgb * mask14.rgb, x2
///     alpha  codes   0 20 20 25   ->  diffuse.a * mask12.a * mask14.a,     x4
///
/// and its program closes on exactly that, `MUL result.color.xyz, R0, c[0].y`
/// with c[0] = {4, 2}. The `cm2x` and `am4x` in its name are the same two
/// numbers.
///
/// **Does this stage's texture feed this channel?** 0 says no, and the
/// non-texture argument forms (2, 41, 43, 71, 80, 82 — a constant or the
/// interpolated colour rather than a sampler) say no. Everything else says yes.
/// 98.4% of the codes that say no have no `TEX` for that channel and ~93% of
/// the ones that say yes have one.
///
/// The tens digit is the OP and the units digit carries the second argument and
/// the output gain. Both are read now, against the shipped programs: the uber
/// reconstructions under `d3_re_shaders/pixel/{Legacy,Billboard}.fx__ps_legacy`
/// are 670 permutations of exactly this block, and over their aligned stages the
/// correspondence below is a bijection --
///
///     tens 0   code 3 the texture alone; code 1 nothing but the vertex colour
///     tens 1   ADD, and it always saturates
///     tens 2   MODULATE by the texture, with the VERTEX COLOUR as argument 2
///     tens 4   no texture: 40 multiplies by the vertex colour, 42 squares
///     tens 8   no texture: the vertex colour is argument 1
///
///     units 0..3 x1   4 x2   5 x4   6 x2 and saturate   7 x4 and saturate   8 saturate
///
/// -- the units column holding under tens 2 and nowhere else.
/// `actor_additive_cm_uv2_appfx_unlit` is what pins that: its colour code is 75
/// and the shipped program modulates at x1, not x4.
/// The four chain operations a stage can perform on a channel.
inline constexpr u8 kD3StageSkip = 0;
inline constexpr u8 kD3StageModulate = 1;
inline constexpr u8 kD3StageAdd = 2;
/// @brief D3DTOP_SELECTARG1 with the TEXTURE as argument 1 — the stage
///        REPLACES the channel, discarding every stage before it.
///
/// Code 3, and it is a different number from a modulate whenever anything came
/// before: `actor_complex_Transparent_Ground` codes its colour block
/// `3 3 3 10` over stages (12, 14, 1, 6) and its shipped program is
/// `saturate(tex1.rgb + tex6.rgb)` — the type-12 and type-14 samples do not
/// appear in it at all. Read as a modulate it multiplies two masks into the
/// albedo that the original throws away. 84 of the corpus's stage codes sit
/// after a stage that does feed the channel, which is where the two readings
/// part; on the other 641 the chain head is 1 and they agree.
inline constexpr u8 kD3StageReplace = 3;

/// @brief `$texDepth` — the scene depth `SoftBillboard.fx` binds for its soft
///        fade. It is a render-target read, not one of the material's textures,
///        and the combine block does not count it as a stage.
inline constexpr i32 kD3TextureTypeSceneDepth = 39;

/// @brief `RenderPass::dwUnknown00` is the pass's RENDER PHASE, and phase 3 is
///        the screen-space DISTORTION buffer.
///
/// A distortion surface does not draw colour: it writes a signed screen-space
/// offset into a side buffer that a full-screen pass afterwards bends the
/// finished scene through. Nothing in the pass's own state says so -- the state
/// is an ordinary blended draw -- and the effect file does not either, because
/// only 5 of the 48 shipped distortion passes are `Distortion.fx`. The other 43
/// are `Legacy.fx` / `Billboard.fx` running the same fixed-function chain
/// everything else runs; what they compute is a vector, not a colour.
///
/// The phase is the discriminator, and it is exact. Over all 1,506 shipped
/// `Shaders` assets, the 45 whose `dwShaderFlags` carries bit 3 are EXACTLY the
/// 45 that declare a phase-3 pass -- zero mismatches either way. Bit 3 is what
/// `ActorModel_EmitSubObjectDrawCalls` tests to skip a sub-object when
/// capability 97 is unavailable, and 97 is the same capability the distortion
/// post effect itself is gated on (`sub_744200` case 0), so the two readings
/// close on each other.
///
/// It also picks the right PASS, which no name-based rule can: `actor_mysticAlly`
/// is (6, 3) and `actor_watermonster` is (3, 6, 6) -- the distortion pass is
/// last in one and first in the other.
///
/// The other phases the corpus uses, for orientation: 6 the main pass (1,047),
/// 5 (318), 2 (136), 12 (74), 15 late transparent (68), 8 texture filter (53),
/// 21/23 prepass, 9 shadow cookie, 11 reflection, 17 highlight.
inline constexpr i32 kD3RenderPhaseDistortion = 3;

/// @brief `Shaders::dwShaderFlags` bit 3 -- "this asset declares a distortion
///        pass". Redundant with @ref kD3RenderPhaseDistortion by measurement,
///        and kept because it is the bit the ENGINE tests.
inline constexpr i32 kD3ShaderFlagDistortion = 0x8;

/// @brief The resolve's fixed gain, in viewport UV.
///
/// `v14 PostFX Distortion` is 14 ARB instructions and this is the only constant
/// in it that is not a bias:
///
///     offset = (texDistortion.rg * 2 - 1) * 0.03
///     result = texOriginal(uv + offset)
///
/// So the buffer holds a signed offset encoded around 0.5, a pixel that nothing
/// drew must READ BACK 0.5, and the clear is therefore (0.5, 0.5, 0.5, 0) --
/// the write blends SRCALPHA / INVSRCALPHA over it.
inline constexpr f32 kD3DistortionStrength = 0.03f;

struct D3StageArg {
    bool usesTexture = false; ///< Is this stage's texture sampled for the channel?
    /// @brief ... and MULTIPLIED into the channel, rather than replacing it or
    ///        being added to it?
    ///
    /// `20 20 0 24` is Imperius's wing colour chain and every term in it is a
    /// multiply, while Cain's smoke plume opens `3 3 3 10` — three replaces and
    /// an add — and closes `saturate(diffuse + glow)`. The two programs do not
    /// disagree at all: their passes said different things.
    bool modulates = false;
    /// @brief tens 1 — the stage ADDS its texture into the channel. 45 of the
    ///        corpus's 17,503 particle systems, and always saturating.
    bool adds = false;
    /// @brief tens 0 code 3 — the stage REPLACES the channel. See
    ///        kD3StageReplace.
    bool replaces = false;
    f32 gain = 1.0f;     ///< 1, 2 or 4 — MODULATE, MODULATE2X, MODULATE4X.
    bool clamps = false; ///< Saturate the channel HERE, not once at the end.
};

inline D3StageArg D3ReadStageArg(u32 code) {
    D3StageArg a;
    const u32 tens = code / 10;
    const u32 units = code % 10;
    // The forms whose first argument is not a sampler: a constant (tens 4), the
    // vertex colour (tens 8), or 71. Under tens 0 only code 3 samples -- 0 is
    // the stage saying it does not touch this channel at all, 2 is a constant,
    // and 1 leaves the channel alone (`x1_particle_crusader_fistofheavens_
    // boltsparks` codes it and its program SKIPs stage 0's colour).
    a.usesTexture = tens == 0 ? code == 3 : (code != 71 && tens != 4 && tens != 8);
    a.modulates = a.usesTexture && tens == 2;
    a.adds = a.usesTexture && tens == 1;
    a.replaces = code == 3;
    if (tens == 2) {
        a.gain = (units == 4 || units == 6) ? 2.0f : (units == 5 || units == 7) ? 4.0f : 1.0f;
        a.clamps = units >= 6 && units <= 8;
    } else {
        a.clamps = a.adds;
    }
    return a;
}

/// @brief Does this code take the VERTEX COLOUR as its second argument?
///
/// Only asked of a chain's FIRST active stage, which is the only place D3D9's
/// fixed function reaches the diffuse: a leading `3` starts from the texture
/// alone and the vertex colour never enters at all. 272 of the corpus's 15,841
/// `ps_legacy` particle systems do that.
///
/// **Units 2 is the TEXTURE FACTOR, not the diffuse.** `actor_transparent_
/// edgeAlpha_cm2x2_am4x4_bloom` codes its first colour stage 22 and the
/// reconstruction reads it `MOD|FACTOR`, against the `MOD|VCOLOR` its
/// units-0 neighbours get; `actor_complex_Transparent_Ground` does the same in
/// the alpha block. See D3StageTakesFactor. Units 3 is unmeasured (17 codes in
/// the whole corpus) and stays on this side.
inline bool D3StageTakesVertexColor(u32 code) {
    return code / 10 == 2 && code % 10 != 2;
}

/// @brief Does this code take the TEXTURE FACTOR as its second argument?
///
/// D3DTA_TFACTOR, which is the `Factor` shader constant — and `Factor` is the
/// DRAW's colour, not the pass's: `Render_SetPassConstantColor` is called with
/// `renderRecord + 8` immediately after `Render_BindShaderPass` on all three
/// draw paths, so the pass's own `dwConstantColor` is overwritten before the
/// first flush. In a viewer the record carries no tint and the sub-object's
/// fade alpha, i.e. `(1, 1, 1, elementAlpha)`.
inline bool D3StageTakesFactor(u32 code) {
    return code / 10 == 2 && code % 10 == 2;
}

/// @brief Code 40 — a stage with no texture that multiplies the channel by the
///        vertex colour, i.e. the colour entering LAST instead of first.
///
/// 411 particle systems, and `particle_transparent_blizzard` pairs it with a
/// leading `20` so the colour enters twice.
inline bool D3StageIsVertexColorOnly(u32 code) {
    return code == 40;
}

/// @brief Code 41 — the same, for the TEXTURE FACTOR.
///
/// 173 codes, and `actor_seismicSlam_wave` is why it had to be read: its
/// four-stage alpha block ends `20 20 25 41` on a pass whose fourth stage is a
/// type-0 hole, and the reconstruction folds that 41 into the third stage as
/// `MOD_4X|FACTOR`. In the colour block `Factor.rgb` is white here, so this is
/// visible in the alpha alone — where it is the actor's fade.
inline bool D3StageIsFactorOnly(u32 code) {
    return code == 41;
}

/// @brief Code 43 — a post-multiply by `appearanceFX.x`.
///
/// `sub_746D10` builds that constant from four SURFACE tags (0x30300..0x30303)
/// as a periodic pulse; with the tag absent the mode is 0 and it stays at its
/// initial `1.0`, which no shipped model this renderer opens overrides. So this
/// is a no-op that is *known* to be a no-op rather than a code being ignored.
/// 36 codes. (42, one code, SQUARES the channel and is not reproduced.)
inline bool D3StageIsAppFxOnly(u32 code) {
    return code == 43;
}

/// @brief The tag ids of the two combine groups. Six stages each, consecutive,
///        which is what lets a caller walk them as `base + i`. The shader-tag
///        registry names them TAG_VS_PS0C_FUNC..TAG_VS_PS5C_FUNC ("Stage 1..6
///        Color Function") and TAG_VS_PS0A_FUNC..TAG_VS_PS5A_FUNC.
inline constexpr u32 kD3TagStageColor = 0xA0016u;
inline constexpr u32 kD3TagStageAlpha = 0xA001Cu;
inline constexpr u32 kD3StageArgCount = 6;

/// @brief The alpha code that marks the EROSION tail, and nothing else.
///
/// `ps_legacy`'s dissolve arm closes with
/// `alpha = min(1, pow(alpha, 10 * COLOR1.a))`, which is what eats a `_wipe` /
/// `_dissolve` / `_errosion` sprite away. It sits in the alpha block PAST the
/// content stages, so it is not a combine at all: 86 is a marker.
///
/// Exact over the corpus's 245 billboard passes: only 16 carry a tens-8 alpha
/// code at all. Eleven of those carry 86, and those eleven are precisely the
/// reconstruction's `EROSION` permutations; the other five carry (80, 81) and
/// mark the BLENDADD arm instead.
inline constexpr u32 kD3StageAlphaErosion = 86u;

/// @brief `TAG_VS_TEXCOORD0..5_FUNC` — what the vertex stage does with texcoord
///        block slot `i`, and whether the slot exists at all.
///
/// Decoded by joining the `vs_legacy` reconstruction's `NUM_TEXCOORDS` /
/// `TCk_UV` / `TCk_MAT` / `TCk_MODE` defines against the passes that name each
/// permutation — 223 shaders — and the mapping is exact once two things are
/// right about the block itself:
///
///  1. **An absent tag takes the registry's default**, which is 1, 1, 1, 8, 10,
///     10 for the six. Twelve shaders (`particle_additive_am4x_alphatest` and
///     friends) leave the tail unset and their programs still emit four
///     texcoords; read as "absent means disabled" they came out two wide.
///  2. **The vertex program's output slots are the LIVE tags, compacted.**
///     `particle_blendAdd` tags `(1, 10, 10, 1, 11, 11)` and emits four, so its
///     fourth output is block slot 5. The block index is not the output index.
///
/// With those, over every slot of every named permutation:
///
///     value   uv set   texture matrix        count
///        1       0     matTex[block index]   227
///        8       1     matTex[block index]   183
///       13       2     matTex[block index]   105
///       15       3     matTex[block index]    39
///       11       0     matTex0                13
///        0       0     none - RAW             10
///       12       2     none - RAW              2
///       14       3     none - RAW             21
///       10       -     the slot does not exist
///
/// no residue in any column.
///
/// **The uv SET is the live half and the matrix is inert**, which is the
/// opposite of how this first read. Two measurements say so:
///
///  * `particle_additive_am4x_alphatest` tags slot 2 with code **1**, not 13.
///    Both name `matTex[2]`, so under a matrix reading the author's choice
///    would mean nothing; they differ only in the uv set (0 against 2).
///  * `Particle_DrawBatch` uploads no texture matrix at all. Geometry sends a
///    stage's transform through `MatTex_UploadUvMatrix` as a 4x4; a particle
///    gets the same transform from the same uv state as a 2x3 BAKED into its
///    vertices by `Particle_WriteQuadVertices`. They are two delivery paths for
///    one transform, so a particle's `matTex` is the identity and RAW and
///    matrixed come to the same thing.
///
/// And the sets are reachable: uv set k is the baked transform of the stage
/// whose type is `kD3TexcoordSetType[k]`, which is a layer this build already
/// carries. So a slot reads the transform its code names, not its own.
///
/// 17 and 18 also occur, on six `Legacy.fx` actor shaders that no `.prt`
/// reaches. They are read as live, own-matrix, not raw.
inline constexpr u32 kD3TagTexcoordFunc = 0xA0010u;

/// The registry defaults for the six slots, applied to an absent tag.
inline constexpr u32 kD3TexcoordDefault[6] = {1u, 1u, 1u, 8u, 10u, 10u};

/// @brief Does the vertex program emit this slot at all? 10 says no.
inline bool D3TexcoordIsLive(u32 code) {
    return code != 10u;
}

/// @brief The vertex uv set a live slot reads, 0-3.
///
/// 17 and 18 occur only on `Legacy.fx` actor shaders no `.prt` reaches; they
/// fall through to set 0 rather than assert, because a surface never asks.
inline u32 D3TexcoordUvSet(u32 code) {
    switch (code) {
    case 8u:
        return 1u;
    case 12u:
    case 13u:
        return 2u;
    case 14u:
    case 15u:
        return 3u;
    default:
        return 0u;
    }
}

/// @brief The texture TYPE each vertex uv set carries the transform of.
///
/// `Particle_PrepareSystemDrawRecord` fills the draw record's four uv-state
/// slots by type in this order and leaves a hole where a type is absent, so
/// the set index is positional and not the material's compacted layer index.
/// An absent type bakes the identity rectangle.
inline constexpr i32 kD3TexcoordSetType[4] = {1, 19, 12, 14};

/// Codes 0, 12 and 14 pass the vertex uv through with no matrix at all. Inert
/// for a particle, which never has a matrix to skip — kept because it is what
/// the enum means and a surface family may yet need it.
inline bool D3TexcoordIsRaw(u32 code) {
    return code == 0u || code == 12u || code == 14u;
}

/// @brief Which `Billboard.fx` pixel program a pass runs, reduced to the three
///        shapes this build implements.
///
/// `szPixelShaderEntry` names the program outright, so this is a lookup and not
/// an inference. Nine of the twelve entry points the corpus's `.prt` reach are
/// one of these; see D3_MATERIAL_AUDIT.md §12.
enum class D3ParticleProgram : u8 {
    /// `ps_legacy` and the flow shaders: the fixed-function combine chain.
    Chain = 0,
    /// `ps_blend_add*` / `ps_billboard_blendAdd_flowMult`: two premultiplied
    /// layers summed, which is not a chain and cannot be written as one.
    BlendAdd = 1,
    /// `ps_particle_water_sim`: the diffuse scaled by the vertex ALPHA alone.
    WaterSim = 2,
};

/// @brief The engine's nominal tick, and the unit every authored UV rate is in.
///
/// `ActorModel_ResolveSubObjectMaterials` steps its first pose with a literal
/// 0.016667, and `flFramesPerTick` is already read as `x 60` by the clip
/// sampler. Measured over 13,857 non-zero rates: 99.8% are exact to three
/// decimals once multiplied by 60, against 8.6% as authored — so the stored
/// value is per tick and this is what turns it into per second.
inline constexpr f32 kD3TicksPerSecond = 60.0f;

// ---------------------------------------------------------------------------
// The cooked convex polytope behind `CollisionShape::arPolytopeData`.
//
// A `CollisionShape` of kind 2 -- 25,775 of the corpus's 36,861, so the
// majority -- carries no radius and no endpoints; its geometry is an offline
// cook that `PhysicsBridge_CreateFixture` hands to Domino whole. WhiteoutLib
// resolves the shape's own reference and stops, because the header it lands on
// contains four MORE references and the generator's type table has no way to
// say that. So the second level is read here.
//
// The header is **exactly 96 bytes on every one of the 25,775**, which is what
// says it is a fixed record rather than a variable point cloud:
//
//     +0   32 bytes of zero -- eight runtime pointer slots, blanked on disk
//     +32  Vector3f  the centroid
//     +44  u32       vertex count
//     +48  u32       face count
//     +52  u32       HALF-edge count
//     +56  f32       volume  <- the field the client validates, >= 4.4143e-6
//     +60  f32       unrecovered (a second positive scalar)
//     +64  (i32 offset, i32 size)  vertices,   12 bytes each  (Vector3f)
//     +72  (i32 offset, i32 size)  face planes, 16 bytes each (Vector4f n,d)
//     +80  (i32 offset, i32 size)  half-edges,  4 bytes each
//     +88  (i32 offset, i32 size)  face -> first half-edge, 1 byte each
//
// The offsets are **payload-relative** -- measured from `file + 16`, past the
// SNO preamble -- which is the same space every other D3 reference lives in and
// the single thing that makes the arrays read as garbage if missed.
//
// The three counts satisfy Euler exactly -- V + F = E/2 + 2 on every sample
// read (8/6/24, 22/15/70, 32/18/96, 7/7/24) -- and each of the four sizes
// divides by its count to the stride above with no remainder. Together that
// identifies the record; no single field would.
//
// A half-edge's four bytes, measured over 17,344 of them in 400 shapes:
//
//     lane 0   UNRECOVERED. Exactly 50.0% of values fall below each of V, F
//              and E, which is what a *non*-index looks like; a twin would be
//              100% below E. Nothing here needs it.
//     lane 1   the ORIGIN vertex -- 100.0% below V
//     lane 2   the FACE          -- 100.0% below F
//     lane 3   `next` around the face -- 100.0% below E, and over the whole
//              array a PERMUTATION of 0..E-1 on 400 shapes out of 400, which is
//              what a next-pointer is and what a twin is not
//
// So an undirected edge list is `(origin[i], origin[next[i]])` deduplicated,
// and that needs neither lane 0 nor the face planes.
struct D3Polytope {
    Vector3f centroid{0.0f, 0.0f, 0.0f};
    f32 volume = 0.0f;
    std::vector<Vector3f> points;
    /// @brief Vertex index pairs, one pair per undirected edge.
    std::vector<u16> edges;
};

inline constexpr usize kD3PolytopeHeaderBytes = 96;
/// @brief The client rejects a cook whose volume is below this or non-finite.
inline constexpr f32 kD3PolytopeMinVolume = 4.4143e-6f;

/// @brief Bytes of SNO preamble ahead of every struct image and payload.
inline constexpr usize kD3SnoPreambleBytes = 16;

/// @brief Read the cook at @p header out of @p file, or nullopt.
///
/// @param header the 96 bytes of `CollisionShape::arPolytopeData`
/// @param file   the whole `.app` the shape came from — the four references
///               are payload offsets and mean nothing without it
inline std::optional<D3Polytope> D3ReadPolytope(std::span<const u8> header,
                                                std::span<const u8> file) {
    if (header.size() != kD3PolytopeHeaderBytes || file.size() <= kD3SnoPreambleBytes)
        return std::nullopt;
    const auto word = [&](usize i) {
        i32 v = 0;
        std::memcpy(&v, header.data() + i * 4, 4);
        return v;
    };
    const auto real = [&](usize i) {
        f32 v = 0.0f;
        std::memcpy(&v, header.data() + i * 4, 4);
        return v;
    };

    const auto nv = static_cast<u32>(word(11));
    const auto ne = static_cast<u32>(word(13));
    const f32 volume = real(14);
    // The client's own gate, verbatim: a cook below this volume, or one whose
    // volume is not finite, builds no fixture at all.
    if (!(volume >= kD3PolytopeMinVolume) || !std::isfinite(volume))
        return std::nullopt;
    // A convex solid is at least a tetrahedron. Below that the arrays are not
    // wrong so much as meaningless, and a 1-point "hull" draws as a dot.
    if (nv < 4 || ne < 6)
        return std::nullopt;

    const auto payload = file.subspan(kD3SnoPreambleBytes);
    const auto arrayAt = [&](usize refWord, u32 stride, u32 count) -> const u8* {
        const i32 off = word(refWord);
        const i32 size = word(refWord + 1);
        if (off <= 0 || size <= 0 || static_cast<u32>(size) != count * stride)
            return nullptr;
        const auto o = static_cast<usize>(off);
        const auto n = static_cast<usize>(size);
        if (o > payload.size() || n > payload.size() - o)
            return nullptr;
        return payload.data() + o;
    };

    const u8* pts = arrayAt(16, 12, nv);
    const u8* hes = arrayAt(20, 4, ne);
    if (!pts || !hes)
        return std::nullopt;

    D3Polytope out;
    out.centroid = {real(8), real(9), real(10)};
    out.volume = volume;
    out.points.resize(nv);
    for (u32 i = 0; i < nv; ++i) {
        f32 c[3];
        std::memcpy(c, pts + i * 12, 12);
        out.points[i] = {c[0], c[1], c[2]};
    }

    // `(origin[i], origin[next[i]])`, deduplicated as an unordered pair. Every
    // edge is walked twice — once per half — so without the dedupe a wireframe
    // draws each line on top of itself.
    std::vector<u32> keys;
    keys.reserve(ne);
    for (u32 i = 0; i < ne; ++i) {
        const u32 a = hes[i * 4 + 1];
        const u32 nxt = hes[i * 4 + 3];
        if (nxt >= ne)
            continue;
        const u32 b = hes[nxt * 4 + 1];
        if (a >= nv || b >= nv || a == b)
            continue;
        const u32 lo = a < b ? a : b;
        const u32 hi = a < b ? b : a;
        keys.push_back((lo << 16) | hi);
    }
    std::sort(keys.begin(), keys.end());
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
    out.edges.reserve(keys.size() * 2);
    for (const u32 k : keys) {
        out.edges.push_back(static_cast<u16>(k >> 16));
        out.edges.push_back(static_cast<u16>(k & 0xFFFFu));
    }
    return out;
}

/// @brief One texture entry's UV transform, read out of the 144-byte block.
///
/// `MatTex_BuildUvMatrix4x4` is the mesh half and `MatTex_BuildUvAffine2x3`
/// @0x71000F8FF0 the particle half; G-D3P-16 runs the second and they agree
/// except where noted. Mode 1 copies an authored transform verbatim from the
/// block's +4 (entry 0x10), OFF-DIAGONALS INCLUDED; mode 2 builds
/// `scale * rotate` with the scale at block +4 and +24 (entry 0x10 and 0x24) and
/// takes rotation and translation from the per-draw animation state, pivoting
/// the rotation about the scroll offset re-centred on 0.5 when the flags word
/// has bit 3. Mode 3 is an `Anim2D` frame table (§19.4) and mode 4 is an
/// ENVIRONMENT-MAP projection — the render system's view matrix with its
/// translation row stripped, inverted, first three columns negated (G-D3P-16).
/// Neither of those, nor the two bone modes, is reproduced here.
struct D3UvXform {
    D3UvMode mode = D3UvMode::Identity;
    Vector2f scale = {1.0f, 1.0f};
    Vector2f offset = {0.0f, 0.0f};      ///< Constant, from flAmount.
    Vector2f scrollPerSec = {0.0f, 0.0f}; ///< From flRate0, already x60.
    f32 rotate = 0.0f;
    f32 rotatePerSec = 0.0f;
    bool aboutCentre = false;
    /// `tAnim3.flAmount`, tested as an integer by
    /// `MatTex_TickUvStateEntry` @0x71000F8310: set, the scroll is CLAMPED into
    /// [0,1] instead of wrapped, which turns a loop into a one-way reveal.
    bool clampUv = false;
    /// @brief Mode 3 only: does the flip-book supply its own tile SCALE?
    ///
    /// `tAnim4.flAmount` (entry 0x80), tested as an INTEGER by
    /// `MatTex_BuildUvAffine2x3` case 3 -- so `-0.0f` is set, and the golden's
    /// `atlas: -0.0` case is a code 3 for exactly that reason. Clear, the case
    /// writes only a translation and returns 1 (the caller adds it to the base
    /// rectangle); set, it also writes `diag(frame0.u1, frame0.v1)` and returns
    /// 3. Shipped on 667 of the corpus's 8,390 mode-3 particle entries, 527 of
    /// them on a stage OTHER than 0 -- which is what it is for: only stage 0's
    /// base rectangle is the tile, so a flip-book anywhere else has to carry
    /// its own size.
    bool atlasScale = false;
    /// @brief UV mode 1's authored 2x3, in @ref D3UvAffine's own output order.
    ///
    /// `MatTex_BuildUvAffine2x3` @0x71000F8FF0 case 1 copies six floats from the
    /// entry verbatim — `vUvRow0.xy`, `vUvRow1.xy` and `vUvRow3.xy` — so mode 1
    /// is a full authored transform including its off-diagonals, not a scale
    /// pair. Left at identity when the block is all zero, which is what an entry
    /// with no authored matrix carries and which would otherwise collapse every
    /// texture coordinate to the origin. Same guard as the mesh path's.
    f32 authored[6] = {1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f};
    /// @brief `flRate1` of the U, V and rotation triples, already x60.
    ///
    /// A PER-INSTANCE rate jitter: `MatTex_InitUvState` draws once per stage
    /// instance and stores `flRate0 + flRate1 * U`, and takes the draw only when
    /// flRate1 is non-zero (which is what keeps the stream aligned). For a
    /// particle system the instance is the PARTICLE, so this is what stops every
    /// member of a puff scrolling in lockstep. 9,235 shipped mode-2 entries
    /// carry one.
    Vector2f scrollJitter = {0.0f, 0.0f};
    f32 rotateJitter = 0.0f;
    /// @brief Bits 0 and 1 of the flags word: draw the initial U / V phase at
    ///        RANDOM instead of taking it from `flAmount`. Mode 2 only.
    ///
    /// 12,361 of the corpus's 13,897 mode-2 entries set one or both, across
    /// 9,981 files — so on the majority of shipped particle materials the
    /// starting coordinate is per particle and there is no shared phase to
    /// evaluate. See kD3UvFlagRandomPhaseU.
    bool randomPhaseU = false;
    bool randomPhaseV = false;
    /// @brief Does anything here move? Both the surface table and the adapter
    ///        ask, and they must agree on the answer or the palette id one
    ///        assigns names an entry the other never publishes.
    bool animated = false;
};

/// The engine's clamp-then-wrap, `MatTex_InitUvState` @0x71000F7C60: pin to
/// 8x2pi first, then fold into [0, 2pi] a turn at a time.
inline f32 D3WrapAngle(f32 a) {
    constexpr f32 kTwoPi = 6.28318530717958647692f;
    constexpr f32 kClamp = kTwoPi * 8.0f;
    a = (a < -kClamp) ? -kClamp : ((a > kClamp) ? kClamp : a);
    while (a < 0.0f)
        a += kTwoPi;
    while (a > kTwoPi)
        a -= kTwoPi;
    return a;
}

/// The engine's per-frame UV fold, `MatTex_TickUvStateEntry` @0x71000F8310.
///
/// There it is a loop of `+1` / `-1` run on a value that never leaves [0,1]
/// before the step, so it terminates in an iteration or two. Evaluated in closed
/// form the argument can be arbitrarily large, so this folds with `fmod` — the
/// same number without the loop, with one wrinkle worth keeping: the engine's
/// boundary test is a STRICT `> 1.0`, so a whole number folds to 1.0 where `fmod`
/// would say 0.
inline f32 D3FoldUv(f32 v) {
    if (v >= 0.0f && v <= 1.0f)
        return v;
    f32 r = std::fmod(v, 1.0f);
    if (r < 0.0f)
        r += 1.0f;
    if (r == 0.0f)
        return v > 0.0f ? 1.0f : 0.0f;
    return r;
}

/// @brief Read @p e's UV transform. The single reader; nothing else touches
///        `vUvRow*` or the anim triples.
inline D3UvXform D3ReadUvXform(const d3n::MaterialTextureEntry& e) {
    D3UvXform x;
    x.mode = static_cast<D3UvMode>(D3UvModeOf(e));
    x.aboutCentre = (D3UvFlagsOf(e) & kD3UvFlagRotateAboutCentre) != 0;
    if (x.mode == D3UvMode::ScaleRotateScroll) {
        // The engine clamps BOTH to 1 when either is non-positive, rather than
        // clamping them independently — an entry that authored only one scale
        // gets neither.
        if (e.vUvRow0.x > 0.0f && e.vUvRow1.y > 0.0f) {
            x.scale = {e.vUvRow0.x, e.vUvRow1.y};
        }
    } else if (x.mode == D3UvMode::Matrix) {
        const Vector4f rows[4] = {e.vUvRow0, e.vUvRow1, e.vUvRow2, e.vUvRow3};
        bool authored = false;
        for (const Vector4f& v : rows) {
            if (v.x != 0.0f || v.y != 0.0f || v.z != 0.0f || v.w != 0.0f)
                authored = true;
        }
        if (authored) {
            x.authored[0] = e.vUvRow0.x;
            x.authored[1] = e.vUvRow1.x;
            x.authored[2] = e.vUvRow3.x;
            x.authored[3] = e.vUvRow0.y;
            x.authored[4] = e.vUvRow1.y;
            x.authored[5] = e.vUvRow3.y;
        }
    }
    // `flRate0` is the scroll and `flAmount` a constant phase — the 736 channels
    // that set only flAmount are a static UV shift. `flRate1` is a per-instance
    // RATE JITTER and flags bits 0/1 randomise the initial phase; both are read
    // now (@ref D3UvXform::scrollJitter, @ref D3UvXform::randomPhaseU) because
    // for a particle the "instance" is the PARTICLE, and leaving them out drew
    // every member of a system on one shared coordinate.
    x.offset = {e.tAnimU.flAmount, e.tAnimV.flAmount};
    x.scrollPerSec = {e.tAnimU.flRate0 * kD3TicksPerSecond,
                      e.tAnimV.flRate0 * kD3TicksPerSecond};
    x.scrollJitter = {e.tAnimU.flRate1 * kD3TicksPerSecond,
                      e.tAnimV.flRate1 * kD3TicksPerSecond};
    x.rotateJitter = e.tAnimRotate.flRate1 * kD3TicksPerSecond;
    x.randomPhaseU = (D3UvFlagsOf(e) & kD3UvFlagRandomPhaseU) != 0;
    x.randomPhaseV = (D3UvFlagsOf(e) & kD3UvFlagRandomPhaseV) != 0;
    // The engine clamps the authored rotation to 8x2pi and wraps it into
    // [0, 2pi] before it ever reaches the matrix, so a negative or multi-turn
    // amount is not the angle it looks like.
    x.rotate = D3WrapAngle(e.tAnimRotate.flAmount);
    x.rotatePerSec = e.tAnimRotate.flRate0 * kD3TicksPerSecond;
    // Tested as an INTEGER by the engine, so `-0.0f` is set. Reading it as a
    // float would call the same authored value clear.
    {
        u32 bits = 0;
        std::memcpy(&bits, &e.tAnim3.flAmount, sizeof(bits));
        x.clampUv = bits != 0;
        std::memcpy(&bits, &e.tAnim4.flAmount, sizeof(bits));
        x.atlasScale = bits != 0;
    }
    // Mode 2 alone. It is the only case that reads the per-draw animation
    // state at all — mode 1 copies a fixed matrix and the rest are driven by an
    // Anim2D table, the camera or a bone — so rates on any other entry are dead
    // data, and reporting them as animated would name a palette slot nothing
    // ever fills.
    x.animated = x.mode == D3UvMode::ScaleRotateScroll &&
                 (x.scrollPerSec.x != 0.0f || x.scrollPerSec.y != 0.0f || x.rotatePerSec != 0.0f);
    return x;
}

/// @brief The six affine coefficients of @p x at @p seconds.
///
/// `MatTex_BuildUvMatrix4x4` case 2, verbatim: scale then rotate, translation last, and
/// with the flags' bit 3 the translation is itself pushed through the rotation
/// and re-centred on (0.5, 0.5) so the spin happens about the middle of the
/// tile rather than about its corner.
///
///     u' = a[0]*u + a[1]*v + a[2]
///     v' = a[3]*u + a[4]*v + a[5]
///
/// @p u, @p v and @p rot are mode 2's live animation STATE — the scrolled phase
/// and the accumulated angle. On the geometry path they come from the clock (see
/// the @ref D3UvAffine overload); on the particle path they are the emitting
/// particle's own, because that is where the engine keeps them.
inline void D3UvAffineAt(const D3UvXform& x, f32 u, f32 v, f32 rot, f32 (&a)[6]) {
    // Mode 1 is an authored transform, not a scale pair, and nothing about it is
    // animated. G-D3P-16; without this a mode-1 particle layer got the identity
    // plus whatever tAnimU/tAnimV happened to carry.
    if (x.mode == D3UvMode::Matrix) {
        for (int i = 0; i < 6; ++i)
            a[i] = x.authored[i];
        return;
    }
    // **Only mode 2 reads the animation triples.** Every other case returns a
    // code having written nothing, and the caller then uses the base rectangle
    // unchanged -- G-D3P-16 recorded that ("modes 0 and >=5 ... return a code
    // having written nothing at all") and this fell through to the mode-2 body
    // anyway, so a mode-0 entry scrolled and rotated on data the engine never
    // looks at. 1,488 mode-0 and 733 mode-3 particle entries carry such data,
    // and they are not decoration: `p1_unique_mightyWeapon_remorseless_glow`'s
    // type-19 ALPHA MASK is mode 0 with a 0.5/s diagonal scroll, which slid the
    // mask off the sprite and left a hard-edged rectangle.
    //
    // Mode 3's translation is the flip-book frame's origin and is per PARTICLE,
    // so it cannot be built here; the caller supplies it (and the tile scale
    // @ref D3UvXform::atlasScale asks for). Mode 4 is an environment-map
    // projection and modes 5/6 a bone -- the engine writes an affine for those,
    // this build has neither input, and identity is what it answers with when
    // the bone is unresolved.
    if (x.mode != D3UvMode::ScaleRotateScroll) {
        a[0] = 1.0f; a[1] = 0.0f; a[2] = 0.0f;
        a[3] = 0.0f; a[4] = 1.0f; a[5] = 0.0f;
        return;
    }
    const f32 angle = rot;
    const f32 c = std::cos(angle);
    const f32 sn = std::sin(angle);
    const f32 m00 = c * x.scale.x;
    const f32 m01 = sn * x.scale.x;
    const f32 m10 = -sn * x.scale.y;
    const f32 m11 = c * x.scale.y;
    f32 tu = u;
    f32 tv = v;
    if (x.clampUv) {
        tu = std::clamp(tu, 0.0f, 1.0f);
        tv = std::clamp(tv, 0.0f, 1.0f);
    } else {
        tu = D3FoldUv(tu);
        tv = D3FoldUv(tv);
    }
    if (x.aboutCentre) {
        const f32 ou = tu - 0.5f;
        const f32 ov = tv - 0.5f;
        tu = (ou * m00 + ov * m10) + 0.5f;
        tv = (ov * m11 + ou * m01) + 0.5f;
    }
    // Row-vector on the way out: u' takes the first COLUMN of the engine's
    // basis, because the engine feeds (u, v, 0, 1) from the left too.
    a[0] = m00;
    a[1] = m10;
    a[2] = tu;
    a[3] = m01;
    a[4] = m11;
    a[5] = tv;
}

/// @brief The six coefficients of @p x at @p seconds, for a surface.
///
/// The state a particle keeps per particle, a mesh has one of per material, so
/// the clock stands in for it: phase `flAmount + rate*t`, angle
/// `flAmount + rotRate*t`. The engine folds its state every frame and this is a
/// closed form, so the fold happens inside @ref D3UvAffineAt instead — without
/// it the value grows until the float stops resolving the rate, which is a
/// scroll that visibly stalls in a long session.
///
/// The per-instance rate jitter is deliberately NOT applied here: a surface has
/// one instance and drawing a random number for it would only decorrelate a
/// golden.
inline void D3UvAffine(const D3UvXform& x, f32 seconds, f32 (&a)[6]) {
    D3UvAffineAt(x, x.offset.x + x.scrollPerSec.x * seconds,
                 x.offset.y + x.scrollPerSec.y * seconds,
                 x.rotate + x.rotatePerSec * seconds, a);
}

/// @brief @p x at @p seconds as the 4x4 the shader multiplies from the left.
inline Matrix44f D3UvMatrix(const D3UvXform& x, f32 seconds) {
    f32 a[6];
    D3UvAffine(x, seconds, a);
    Matrix44f m = Matrix44f::identity();
    m.data[0][0] = a[0];
    m.data[1][0] = a[1];
    m.data[3][0] = a[2];
    m.data[0][1] = a[3];
    m.data[1][1] = a[4];
    m.data[3][1] = a[5];
    return m;
}

/// @brief Palette index for geoset @p g's slot @p slot.
///
/// Dense rather than packed so the surface table and the adapter arrive at the
/// same number without walking the same list in the same order — the id is a
/// function of (geoset, slot) and of nothing else. Slots that never animate
/// simply leave their entry at identity, which is what RenderModel fills the
/// palette with.
///
/// Three bands per geoset, not two: a material with a distortion pass has TWO
/// chains, and the same stage position in each is a different layer.
/// `actor_mysticAlly` scrolls `causticDistortion` on its distortion pass alone.
inline constexpr u32 kD3UvTransformStride = kD3SlotCount + 2 * kD3MaxChainStages;

inline i32 D3UvTransformId(usize g, D3SlotKind slot) {
    return static_cast<i32>(g * kD3UvTransformStride + static_cast<u32>(slot));
}

/// @brief The same, for stage @p stage of a `Legacy.fx` chain.
///
/// A chain stage cannot borrow the slot key: its type is a join key with no
/// slot at all on the ids that matter — `actor_seismicSlam_wave` scrolls types
/// 11 and 13, and neither is in @ref D3SlotOfType. So the palette gets a second
/// band per geoset, and both sides index it by the stage's POSITION in the
/// pass's declaration order, which is the one thing the adapter and the surface
/// table can both compute.
inline i32 D3UvTransformIdForStage(usize g, u32 stage) {
    return static_cast<i32>(g * kD3UvTransformStride + kD3SlotCount + stage);
}

/// @brief The same again, for the DISTORTION pass's chain.
///
/// Its own band because a sub-object can bind both chains at once and the two
/// disagree about what stage 0 is: `actor_mysticAlly` is (1, 4) on its scene
/// pass and (0, 10, 6, 12) on its distortion pass, and only the second scrolls.
inline i32 D3UvTransformIdForDistortionStage(usize g, u32 stage) {
    return static_cast<i32>(g * kD3UvTransformStride + kD3SlotCount + kD3MaxChainStages + stage);
}

/// @brief Where an emitted geoset's SubObject lives.
///
/// `geosetId` is an index into the adapter's emission order and this is what it
/// names. Carried rather than re-derived so every per-geoset accessor takes the
/// same skips.
struct D3SubObjectRef {
    u32 geoSet = 0; ///< 0 = tGeoSet0, 1 = tGeoSet1.
    u32 index = 0;
};

} // namespace whiteout::flakes::io
