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
// 0x71001DD980, `sub_71000F8590`, `sub_71001D49D0`):
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
//     0x0C  UV TRANSFORM MODE      `sub_71000F8590` switches on it, 0..6.
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

/// @brief `sub_71000F8590`'s switch, verbatim.
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
    /// The camera / screen-space projection. Not reproduced.
    Screen = 4,
    /// A bone transform drives the coordinates. Not reproduced.
    Bone = 5,
    BoneAlt = 6,
};

/// @brief Bit 3 of the UV flags word: rotate about (0.5, 0.5) rather than
///        about the origin.
///
/// The one bit `sub_71000F8590` tests (`*(_BYTE *)(a2 + 140) & 8`). Set on 26
/// entries in the corpus.
inline constexpr i32 kD3UvFlagRotateAboutCentre = 0x8;

/// @brief Bits 0 and 1 of the same word: the U and V ADDRESS MODES, set = wrap.
///
/// The same encoding `assets::WrapMode` uses, bit for bit, which is why this is
/// a mask and not a conversion. The evidence is the field's own distribution
/// against the transform mode beside it, over 1.48M corpus entries:
///
///     bits 0 (clamp, clamp)  325,227   of which 308,355 are uv mode 0 or 1
///     bits 3 (wrap,  wrap) 1,139,731   of which 1,139,130 are uv mode 2
///     bits 1 (wrap,  clamp)   14,930   of which  14,846 are uv mode 2, 74%
///                                       of them scrolling in U alone
///     bits 2 (clamp, wrap)       384
///
/// A layer with a fixed matrix or no transform at all clamps; a layer that
/// scrolls wraps, and one that scrolls only in U wraps only in U. That is an
/// address mode and nothing else is.
///
/// Forcing wrap on everything is what tiled Imperius's wing SHAPE mask — uv
/// mode 1, a verbatim 4x4 that walks its coordinates outside the tile — so the
/// tendrils repeated instead of ending where the mask says they end.
inline constexpr i32 kD3UvFlagWrapMask = 0x3;

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
/// Two fields are read out of the code, both measured against the programs
/// shipped beside the assets (see the corpus gate in d3_surface_table_test):
///
///  * **Does this stage's texture feed this channel?** 0 says no, and the
///    non-texture argument forms (2, 41, 43, 71, 80, 82 — a constant or the
///    interpolated colour rather than a sampler) say no. Everything else says
///    yes. 98.4% of the codes that say no have no `TEX` for that channel and
///    ~93% of the ones that say yes have one.
///  * **The output gain.** A units digit of 4 is a x2 and of 5 a x4 — the
///    fixed-function MODULATE2X and MODULATE4X — and the stage gains multiply.
///    Measured 97.0% (colour) and 91.8% (alpha) against the constant the
///    program's final instruction multiplies in.
///
/// The tens digit selects the first argument's source and the rest of the units
/// digit the second's; neither is decoded here, because the shading model does
/// not have the fixed-function chain to put them in. What it has is a texture
/// per slot, and these two answers are what tell it what to do with one.
struct D3StageArg {
    bool usesTexture = false; ///< Is this stage's texture sampled for the channel?
    /// @brief ... and MULTIPLIED into the channel, rather than replacing it or
    ///        being added to it?
    ///
    /// The tens digit is the op class and 2 is the modulate: `20 20 0 24` is
    /// Imperius's wing colour chain and every term in it is a multiply, while
    /// Cain's smoke plume — the other Legacy program that was readable, and the
    /// reason the glow map had no rule — opens `3 3 3 10`, three replaces and
    /// an add, and closes `saturate(diffuse + glow)`. The two programs do not
    /// disagree at all: their passes said different things.
    ///
    /// Only the modulate is acted on. A replace or an add is a chain operation
    /// this shading model has no chain to put it in, so those slots keep the
    /// default their type implies and the shader's own glow term handles the
    /// add.
    bool modulates = false;
    f32 gain = 1.0f; ///< 1, 2 or 4 — MODULATE, MODULATE2X, MODULATE4X.
};

inline D3StageArg D3ReadStageArg(u32 code) {
    D3StageArg a;
    const u32 tens = code / 10;
    const u32 units = code % 10;
    // The forms whose first argument is not a sampler: a constant (tens 4 and
    // the bare 2), the vertex colour (tens 8), or 71. Zero is the stage saying
    // it does not touch this channel at all.
    a.usesTexture = code != 0 && code != 2 && code != 71 && tens != 4 && tens != 8;
    a.modulates = a.usesTexture && tens == 2;
    a.gain = units == 4 ? 2.0f : units == 5 ? 4.0f : 1.0f;
    return a;
}

/// @brief The tag ids of the two combine groups. Six stages each.
inline constexpr u32 kD3TagStageColor = 0xA0016u;
inline constexpr u32 kD3TagStageAlpha = 0xA001Cu;
inline constexpr u32 kD3StageArgCount = 6;

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
/// `sub_71000F8590` is the whole of it: mode 1 copies a 4x4 verbatim from the
/// block's +4 (entry 0x10); mode 2 builds `scale * rotate` with the scale at
/// block +4 and +24 (entry 0x10 and 0x24) and takes rotation and translation
/// from the per-draw animation state, optionally about (0.5, 0.5) when the
/// flags word has bit 3. Modes 3..6 drive the coordinates from an `Anim2D`
/// frame table, the camera or a bone and are not reproduced.
struct D3UvXform {
    D3UvMode mode = D3UvMode::Identity;
    Vector2f scale = {1.0f, 1.0f};
    Vector2f offset = {0.0f, 0.0f};      ///< Constant, from flAmount.
    Vector2f scrollPerSec = {0.0f, 0.0f}; ///< From flRate0, already x60.
    f32 rotate = 0.0f;
    f32 rotatePerSec = 0.0f;
    bool aboutCentre = false;
    /// @brief Does anything here move? Both the surface table and the adapter
    ///        ask, and they must agree on the answer or the palette id one
    ///        assigns names an entry the other never publishes.
    bool animated = false;
};

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
    }
    // `flRate1` is left unread: 1,652 of ~15,700 authored channels set it and
    // nothing recovered says what it adds, so it is better absent than
    // invented. `flRate0` is the scroll and `flAmount` a constant phase — the
    // 736 channels that set only flAmount are a static UV shift.
    x.offset = {e.tAnimU.flAmount, e.tAnimV.flAmount};
    x.scrollPerSec = {e.tAnimU.flRate0 * kD3TicksPerSecond,
                      e.tAnimV.flRate0 * kD3TicksPerSecond};
    x.rotate = e.tAnimRotate.flAmount;
    x.rotatePerSec = e.tAnimRotate.flRate0 * kD3TicksPerSecond;
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
/// `sub_71000F8590` case 2, verbatim: scale then rotate, translation last, and
/// with the flags' bit 3 the translation is itself pushed through the rotation
/// and re-centred on (0.5, 0.5) so the spin happens about the middle of the
/// tile rather than about its corner.
///
///     u' = a[0]*u + a[1]*v + a[2]
///     v' = a[3]*u + a[4]*v + a[5]
inline void D3UvAffine(const D3UvXform& x, f32 seconds, f32 (&a)[6]) {
    const f32 angle = x.rotate + x.rotatePerSec * seconds;
    const f32 c = std::cos(angle);
    const f32 sn = std::sin(angle);
    const f32 m00 = c * x.scale.x;
    const f32 m01 = sn * x.scale.x;
    const f32 m10 = -sn * x.scale.y;
    const f32 m11 = c * x.scale.y;
    f32 tu = x.offset.x + x.scrollPerSec.x * seconds;
    f32 tv = x.offset.y + x.scrollPerSec.y * seconds;
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
inline i32 D3UvTransformId(usize g, D3SlotKind slot) {
    return static_cast<i32>(g * kD3SlotCount + static_cast<u32>(slot));
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
