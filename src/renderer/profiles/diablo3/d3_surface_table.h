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
struct D3PassState {
    bool resolved = false; ///< False = nothing was found; the material flags stand.
    bool blendEnable = false;
    u32 blendSrc = 5; ///< D3DBLEND
    u32 blendDst = 6;
    bool depthWrite = true;
    u32 cull = 2;      ///< D3DCULL: 1 none, 2 CW, 3 CCW.
    u8 alphaRef = 0;   ///< 0..255; 0 = no alpha test.
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
    /// there: 713 of the corpus's 1,831 passes ask for no culling at all.
    bool twoSided = false;
    /// RenderPass+60 x 1/255 in the original — now read from there when the
    /// ShaderMap resolves, and from the material flags when it does not.
    f32 alphaTestThreshold = 0.0f;
    bool alphaBlend = false;
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
D3PassState D3PassStateFor(const d3n::SubObjectAppearance& variant,
                           ::whiteout::flakes::io::D3SnoCache* cache);

/// @brief Which bucket a surface draws in.
///
/// An alpha-blended material is transparent; an alpha-tested opaque one is
/// AlphaKey. The per-frame `alpha < 1` half of §6.3 lives in
/// D3StandardShading::Classify, which is the only thing that sees the actor.
core::SurfaceClass D3ClassifySurface(const D3Surface& surface);

} // namespace whiteout::flakes::renderer::profiles::diablo3
