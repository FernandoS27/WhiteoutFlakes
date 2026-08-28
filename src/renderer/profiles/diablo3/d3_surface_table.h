#pragma once

// ============================================================================
// D3SurfaceTable — one entry per emitted SubObject, its material resolved.
//
// Indexed by GEOSET id, following M3SurfaceTable and not M2SurfaceTable,
// because one SubObject is one geoset is one material: there is no per-batch
// grouping to do.
//
// ---------------------------------------------------------------------------
// The texture entries are not an ordered layer list
//
// An UberMaterial is `{ snoShaderMap, MaterialColors, MaterialTextureEntry[] }`.
// Each entry carries an `EMaterialTextureType` and the engine binds it to
// whichever *stage* of the bound pass declares that type
// (Render_BindShaderPass builds `stageSlotMap[type] = stage` from the pass's
// own stage list; Render_ResolveMaterialTextureStages walks it back). So the
// join between a material and a pass is BY TYPE and never by position — which
// is why the type is a LUT key that never repeats inside one material, and why
// io/d3/d3_types.h owns the enum, the slot map and the reader for it.
//
// The pass is reached through `snoShaderMap`: a `.shm` tag map whose values are
// `Shaders` ids, probed along a fixed chain. That is also where every piece of
// render state lives — see D3PassState.
//
// ---------------------------------------------------------------------------
// Twelve, not sixteen
//
// The engine's texture matrices are matTex0..matTex11 and the per-draw flush
// runs `min(stageCount, 12)` times, which is why stage indices above 11 are
// rejected. This walks at most kD3MaxTextureStages entries for the same reason.
//
// Nothing animated lives here (core/surface_table.h states the split): the
// colours are MaterialColors' bind-pose values, and the Params() seam is where
// the tAnimU/V/Rotate triples land.
// ============================================================================

#include "core/surface_table.h"
#include "core/surface_vocabulary.h"
#include "whiteout/flakes/types.h"

#include "io/d3/d3_types.h"

#include <whiteout/sno/d3/native/d3_native.h>

#include <memory>
#include <span>
#include <string>
#include <vector>

namespace whiteout::flakes::io {
class D3SnoCache;
} // namespace whiteout::flakes::io

namespace whiteout::flakes::renderer::profiles::diablo3 {

namespace d3n = ::whiteout::sno::d3::native;

/// @brief The slots the shading model consumes, in shader order.
///
/// Defined beside the canonical texture list (io/d3/d3_types.h) because the two
/// are one statement: the list is exactly "the textures some slot samples".
using ::whiteout::flakes::io::D3SlotKind;
using ::whiteout::flakes::io::kD3SlotCount;

/// @brief matTex0..matTex11 — the ceiling the per-draw flush enforces.
inline constexpr u32 kD3MaxTextureStages = 12;

/// @brief One MaterialTextureEntry, resolved.
struct D3Slot {
    /// Index into the adapter's CollectD3Textures order; -1 = unresolved, and
    /// the shader falls back to the slot's documented default.
    i32 textureId = -1;
    u8 uvSource = 0; ///< Which of the two UV sets.
    u32 wrapFlags = 0x3;
    /// The engine's matTexN at rest — the transform with the clock stopped.
    Matrix44f uvTransform = Matrix44f::identity();
    /// @brief Index into `RenderableView::texAnimPalette`, or -1 when this slot
    ///        does not move.
    ///
    /// The animated half does not live here — the table is the static one by
    /// contract (core/surface_table.h) — so a scrolling slot names a palette
    /// entry the adapter refills every frame and the shading model prefers it
    /// over `uvTransform`. Same seam `.m3` layers use.
    i32 uvTransformId = -1;
    /// The `EMaterialTextureType` this came from, for diagnostics. Never
    /// switched on outside D3SlotOfType.
    i32 rawType = 0;
    /// @brief Which channels of this sample the surface consumes: bit 0 its
    ///        RGB, bit 1 its ALPHA. Zero = the pass did not say, and the slot
    ///        keeps its family default.
    ///
    /// Only a `Legacy.fx` pass states this, and it states it per stage - see
    /// D3StageArg. It is the difference between "type 14 is an alpha mask" and
    /// what the wing program actually does with type 14, which is multiply both
    /// its rgb AND its alpha in.
    u8 channels = 0;
};

/// @brief `D3Slot::channels` bits.
enum : u8 {
    kD3ChannelRgb = 0x1,
    kD3ChannelAlpha = 0x2,
};

/// @brief The render state a Diablo III material does not carry.
///
/// Blend, cull, depth and the alpha-test reference are all properties of the
/// bound *RenderPass*, reached through the sub-object's ShaderMap: `.shm` is a
/// tag map whose values are `Shaders` ids, `ShaderMap_ResolveShaderOpaque`
/// probes a fixed tag chain and takes the first that resolves, and
/// `Render_ApplyPassRenderState` (0x71001DBC80) replays the pass's fields onto
/// the device. `MaterialColors::dwMaterialFlags` was standing in for all of it
/// and is 0 on most of the content that needs blending — Imperius's wings among
/// them.
///
/// The values are D3D9 enums, which the corpus confirms: cull takes only
/// {1 none, 2 CW, 3 CCW}, the depth compare only {4 LessEqual, 6 GreaterEqual,
/// 8 Always}, the blend op only ADD, and the (src, dst) pairs are led by
/// (5, 6) SrcAlpha/InvSrcAlpha on 1,029 passes and (5, 2) SrcAlpha/One on 283.
///
/// The pass also says which of the material's texture entries are live, and
/// which program consumes them. `arTextureStages` is an ordered list of
/// `EMaterialTextureType` ids — unit *i* is bound the entry whose type is
/// `stages[i]` — and `szEffectFile` / `szVertexShaderEntry` name the program.
/// Both matter: an entry of a type the pass never declares is not bound at all
/// (types 26..38 are declared by none of 2,208 resolved passes), and the vertex
/// colour means something different per program family.
struct D3PassState {
    bool resolved = false; ///< False = nothing was found; the material flags stand.
    bool blendEnable = false;
    u32 blendSrc = 5; ///< D3DBLEND
    u32 blendDst = 6;
    bool depthWrite = true;
    u32 cull = 2;      ///< D3DCULL: 1 none, 2 CW, 3 CCW. Pass 0's, verbatim.

    /// @brief Do these two passes together make ONE two-sided draw?
    ///
    /// D3DCULL has no two-sided value, so content that wants a sheet lit from
    /// both sides ships the same pass twice with opposite windings, the second
    /// raising the tag that negates the normal. Twelve corpus shaders do it,
    /// every one named `cloth_*`, and a build that draws pass 0 alone draws
    /// half of every cape. Set alongside `cull` rather than folded into it, so
    /// the field keeps saying what the asset says.
    bool twoSidedPair = false;
    u8 alphaRef = 0;   ///< 0..255; 0 = no alpha test.

    /// @brief One bit per declared `EMaterialTextureType`. A type is always a
    ///        bit position: the resolve pass indexes a 62-slot array.
    u64 declaredTypes = 0;

    /// @brief Does this pass's vertex program ADD the vertex colour's RGB into
    ///        its light sum?
    ///
    /// True for the static-geometry families — `Scene.fx` and `Prop.fx`, whose
    /// meshes carry the level's baked light in the attribute and ship (0,0,0)
    /// outside a level. False for the actor families, which compute the whole
    /// light in the vertex shader and leave the attribute at (255,255,255);
    /// adding it there would blow every character out by a full unit.
    bool vertexColorLights = false;

    /// @brief Does this pass's vertex program route the vertex colour's ALPHA
    ///        into the interpolated alpha?
    ///
    /// `vs_irrad_*` opens `MOV result.color.w, vertex.attrib[3]`; `vs_scene`
    /// takes a uniform there instead and only the `*_vertalpha* `entries read
    /// the attribute. So this is the ActorIrrad family plus, by name, the
    /// entries that say so.
    bool vertexAlpha = false;

    /// @brief Does this pass's pixel program ADD the glow map (type 6) into its
    ///        light sum?
    ///
    /// The glow map's combine op is a property of the PROGRAM, not of the type,
    /// and the shipped programs do not agree on one:
    ///
    ///   `actor2_opaque_glow_skin`  albedo * (light * 2 + glow * k)   ADD
    ///   `scene_opaque_glow`        albedo * light * 2 + glow * k     ADD
    ///   `actor_glowTendril_..`     albedo * light * glow             MULTIPLY
    ///   `actor_complex_Trans..`    saturate(albedo + glow)           ADD to albedo
    ///
    /// So this is true only where two independent families agree — Scene.fx and
    /// ActorIrrad.fx, which both add it as light. Legacy.fx disagrees with
    /// *itself* across its own two programs, so there is no rule to implement
    /// there and the slot stays empty rather than taking one at random: adding
    /// where the original multiplies washed Imperius's wings from fire to
    /// white smoke.
    bool glowLights = false;

    /// @brief Does this pass receive the scene's lights at all?
    ///
    /// `Render_EnsureShaderVariant` compiles one program per combination of
    /// five clamped light counts, each read from **this pass's own tag map**;
    /// and one further tag turns the whole light block off. `0xA000F` is that
    /// tag, and over the corpus it separates the two perfectly: value 0 on 666
    /// passes, every one of which binds a vertex program with no light block at
    /// all, and value 1 on 114, every one of which has one. Absent on the
    /// remaining 962 and the global default is ON.
    ///
    /// An unlit pass is not "dark": `result.color` is the **vertex colour,
    /// verbatim** (`MOV result.color, vertex.attrib[3]` on 552 of the 637 unlit
    /// assets), so the attribute is the whole light term and the pixel program
    /// multiplies its texture chain by it. Lighting such a surface with the
    /// scene's rig is what left Imperius's wings a dark sheet instead of fire.
    bool lit = true;

    /// `szEffectFile`, kept for the census and for diagnostics. Never switched
    /// on outside D3PassStateFor — the flags above are the switch.
    std::string effectFile;

    /// @brief Did this pass carry the fixed-function stage block at all?
    ///
    /// `Legacy.fx` is the only family this shading model reproduces that does -
    /// all 855 of its corpus passes carry it - and where it is present it
    /// OVERRIDES the slot defaults, because it is the shipped answer and they
    /// are a majority rule. See D3StageArg for the grammar.
    bool stageArgs = false;

    /// @brief Types whose stage feeds the colour / the alpha, one bit per
    ///        `EMaterialTextureType`. Meaningless unless `stageArgs`.
    u64 colorTypes = 0;
    u64 alphaTypes = 0;

    /// @brief The chain's output gain per channel: the product of its stages'
    ///        MODULATE2X / MODULATE4X steps, 1 where nothing scales.
    ///
    /// Imperius's wings are (2, 4) - `cm2x` and `am4x`, which is also what the
    /// shader asset is named - and the alpha half is what makes the tendrils a
    /// sheet reaching the armour rather than a few separated strands: at x1
    /// only the peaks of `diffuse.a * mask12.a * mask14.a` clear the
    /// background, and the wing reads as detached from the model wearing it.
    f32 colorGain = 1.0f;
    f32 alphaGain = 1.0f;
};

struct D3Surface {
    bool valid = false; ///< Resolved to a material with at least one texture.
    /// @brief Vanish below alpha 1 rather than drawing opaque.
    ///
    /// ActorModel_EmitSubObjectDrawCalls picks `rec[24]` (opaque) or `rec[28]`
    /// (translucent) and **skips the sub-object entirely** when the chosen id
    /// is -1. We cannot swap programs we do not have, so the surviving half of
    /// that behaviour is this: a sub-object with no translucent variant
    /// disappears when faded instead of popping to opaque.
    bool noTranslucentVariant = false;
    /// Cull mode is a *pass* property in the original, and now comes from
    /// there: 713 of the corpus's 1,831 passes ask for no culling at all, and
    /// twelve more spell two-sided as a CW pass plus a CCW one.
    bool twoSided = false;
    /// RenderPass+60 x 1/255 in the original — now read from there when the
    /// ShaderMap resolves, and from the material flags when it does not.
    f32 alphaTestThreshold = 0.0f;
    bool alphaBlend = false;
    /// @brief Take no light: the vertex colour IS the light for this surface.
    ///
    /// `pass.lit` with one viewer deviation, made here because it needs the
    /// geometry. A static family's vertex colour is a LEVEL BAKE, and outside a
    /// level it ships (0, 0, 0) — measured 100 of the corpus's 132 unlit
    /// sub-objects, 39 of them `Scene.fx`. Honouring `lit` there would multiply
    /// the surface by black and draw a prop as a hole; the attribute is absent,
    /// not zero. So an all-black attribute falls back to the viewer's rig,
    /// which is the same call the shader's residency comment makes: in a viewer,
    /// missing data must not become an invisible mesh.
    ///
    /// The content this is actually for keeps its answer — Imperius's wings
    /// carry white on all 726 vertices, Malthael's on all 904.
    bool unlit = false;
    /// The pass this sub-object binds, when its ShaderMap resolved.
    D3PassState pass;

    // MaterialColors, verbatim: a fixed-function material and nothing more.
    Vector4f diffuse = {1.0f, 1.0f, 1.0f, 1.0f};
    Vector4f specular = {0.0f, 0.0f, 0.0f, 1.0f};
    Vector4f emissive = {0.0f, 0.0f, 0.0f, 1.0f};
    Vector4f ambient = {0.0f, 0.0f, 0.0f, 1.0f};
    f32 shininess = 20.0f;
    u32 materialFlags = 0;
    /// This sub-object is positioned by one bone rather than skinned. 91% are.
    bool rigid = true;

    D3Slot slots[kD3SlotCount];
};

class D3SurfaceTable final : public core::ISurfaceTable {
public:
    static constexpr core::ProductId kProduct = core::ProductId::D3;

    core::ProductId Product() const override {
        return kProduct;
    }
    usize Count() const override {
        return surfaces_.size();
    }

    /// @brief Surface @p index, or null when out of range — which a draw can
    ///        legitimately hit for an actor mid-reload.
    const D3Surface* Surface(u32 index) const {
        return index < surfaces_.size() ? &surfaces_[index] : nullptr;
    }
    std::vector<D3Surface>& Surfaces() {
        return surfaces_;
    }
    const std::vector<D3Surface>& Surfaces() const {
        return surfaces_;
    }

private:
    std::vector<D3Surface> surfaces_;
};

/// @brief How many entries of each `EMaterialTextureType` a build saw.
///
/// Reported rather than asserted: an unrecognised type is content, and the
/// histogram is what turns "some slot is empty" into a number. The surface-table
/// test prints it over the corpus.
struct D3TypeCensus {
    std::vector<std::pair<i32, usize>> counts; ///< {rawType, entries}, ascending.
    usize unmatchedMaterials = 0;              ///< SubObjects whose name found no material.
    usize entriesPastStageCap = 0;             ///< Beyond matTex11.
};

/// @brief Build the table for @p app under @p lookIndex.
///
/// @p lookByGeoset overrides that index per geoset — a dressed character wears
/// one look per equipment slot, not one per model. Empty (the default) puts
/// every geoset on @p lookIndex, which is every model that is not a character.
///
/// @p emitted is the adapter's emission order (D3ModelAdapter::EmittedSubObjects)
/// — entry g describes geoset g — and @p textures is the same
/// `CollectD3Textures` call `GetTextures()` emits, so the two agree by
/// construction rather than by parallel iteration. @p cache resolves
/// `snoMaterial` variants and may be null. Returns an empty table rather than
/// null, so callers never branch on one.
std::unique_ptr<D3SurfaceTable>
BuildD3SurfaceTable(const d3n::Appearances& app, u32 lookIndex,
                    std::span<const ::whiteout::flakes::io::D3TextureRef> textures,
                    std::span<const ::whiteout::flakes::io::D3SubObjectRef> emitted,
                    ::whiteout::flakes::io::D3SnoCache* cache,
                    std::span<const u32> lookByGeoset = {}, D3TypeCensus* census = nullptr);

/// @brief Resolve @p variant's ShaderMap to the render state of its first pass.
///
/// Returns an unresolved state — every caller then keeps the material flags —
/// when there is no cache, no ShaderMap, or nothing on the tag chain.
/// @brief Is this Shaders one two-sided draw written as two passes?
///
/// Exposed for the corpus gate that pins the rule's population;
/// `D3PassStateFor` is what applies it. See `D3PassState::twoSidedPair`.
bool D3IsTwoSidedPassPair(const d3n::Shaders& shaders);

D3PassState D3PassStateFor(const d3n::SubObjectAppearance& variant,
                           ::whiteout::flakes::io::D3SnoCache* cache);

/// @brief Which bucket a surface draws in.
///
/// An alpha-blended material is transparent; an alpha-tested opaque one is
/// AlphaKey. The per-frame `alpha < 1` half of §6.3 lives in
/// D3StandardShading::Classify, which is the only thing that sees the actor.
core::SurfaceClass D3ClassifySurface(const D3Surface& surface);

} // namespace whiteout::flakes::renderer::profiles::diablo3
