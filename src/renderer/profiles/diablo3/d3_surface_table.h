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
// Each entry carries a `dwTextureType` — an `EMaterialTextureType` — and the
// engine binds it to whichever *stage* of the bound pass declares that type
// (Render_GetTextureStageSlot -> Render_SetTextureStageAsset). The enum's names
// were stripped from the build, but every branch of
// Render_ResolveMaterialTextureStages (0x71001DD980) names itself through the
// core asset it falls back to:
//
//   2         Lightmap    -> default_lightmap (152)
//   3, 47-52  NormalMap   -> flat_NM (96), the IDENTITY default
//   8         Irradiance  -> irradiance (151)
//   21 Vignette (144)  24 ShadowMask (150)  56 DyeRamp (154)  59 BannerDye (155)
//   7, 9, 39, 60, 61   engine render targets (39 = the scene-colour copy)
//   20, 22, 23, 53     computed per draw, no static default  [unresolved]
//   default            the entry's own snoTexture
//
// So this is a slot table keyed by an **enum class**, never by a raw int:
// EMaterialTextureType (stage types) and eShaderConstant (uniform slots)
// overlap numerically and mean nothing to each other. Stage type 39 is the
// scene-colour copy; eShaderConstant 39 is SpecularPower.
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
/// An enum class rather than the raw `dwTextureType`, deliberately — see the
/// header note about the two overlapping small-int namespaces. Appending only:
/// a slot's ordinal is the register its texture binds to.
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
    Count,
};
inline constexpr u32 kD3SlotCount = static_cast<u32>(D3SlotKind::Count);

/// @brief matTex0..matTex11 — the ceiling the per-draw flush enforces.
inline constexpr u32 kD3MaxTextureStages = 12;

/// @brief One MaterialTextureEntry, resolved.
struct D3Slot {
    /// Index into the adapter's CollectD3Textures order; -1 = unresolved, and
    /// the shader falls back to the slot's documented default.
    i32 textureId = -1;
    u8 uvSource = 0; ///< Which of the two UV sets.
    u32 wrapFlags = 0x3;
    /// vUvRow0..3 — the engine's matTexN. Identity for the overwhelming
    /// majority; carried because a scrolling material is otherwise silently
    /// still.
    Matrix44f uvTransform = Matrix44f::identity();
    /// The raw dwTextureType this came from, for diagnostics only. Never
    /// switched on — that is what D3SlotKind is for.
    i32 rawType = 0;
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
    /// Cull mode is a *pass* property in the original, not a material one, so
    /// v1 culls back faces uniformly and this is the per-surface override for
    /// the inevitable two-sided cape.
    bool twoSided = false;
    /// RenderPass+60 x 1/255 in the original. We have no RenderPass, so this
    /// comes from the material flags and defaults to 0 (off).
    f32 alphaTestThreshold = 0.0f;
    bool alphaBlend = false;

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

/// @brief How many entries of each raw `dwTextureType` a build saw.
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

/// @brief Which bucket a surface draws in.
///
/// An alpha-blended material is transparent; an alpha-tested opaque one is
/// AlphaKey. The per-frame `alpha < 1` half of §6.3 lives in
/// D3StandardShading::Classify, which is the only thing that sees the actor.
core::SurfaceClass D3ClassifySurface(const D3Surface& surface);

} // namespace whiteout::flakes::renderer::profiles::diablo3
