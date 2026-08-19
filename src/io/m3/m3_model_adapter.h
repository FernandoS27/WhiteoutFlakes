#pragma once

// ============================================================================
// M3ModelAdapter — StarCraft II / Heroes of the Storm `.m3`, geometry only.
//
// Positions and indices from one mesh division's regions, and nothing else: no
// bones, no textures, no materials. Every surface is drawn by UnlitShading, so
// "the model loads" is a visible, gate-able claim before any of the material
// work exists (REFACTOR_PLAN.md P10).
//
// ---------------------------------------------------------------------------
// No sibling resolution, and that is the whole difference from `.m2`
//
// M2 needed two file-system shims because a model is `.m2` + `.skin` (+ `.skel`
// / `.anim`), resolved either by fileDataID or by path. M3 is one self-contained
// chunked file: `m3::Parser::parse(std::span<const u8>)` is the entire load.
// So this adapter takes bytes and never touches IContentProvider — the
// ContentRef is carried only to name the model in diagnostics.
//
// ---------------------------------------------------------------------------
// The vertex stride, which the plan called this phase's real work
//
// It is not, any more. `MODL.vertexFlags` drives a per-vertex stride of
// `24 + (hasColor ? 4 : 0) + numUVs * 4 + 4`, and getting it wrong unpacks
// nothing — but WhiteoutLib's `m3::VertexBuffer` already computes it in
// `initialize()` (called by the parse visitor) and exposes typed accessors.
// `getPositions()` is the whole of what this phase consumes. The work that
// remains is the index indirection below, which the parser does *not* do.
//
// ---------------------------------------------------------------------------
// MODL → DIV_ → REGN → BAT_, and which of those actually selects geometry
//
// A division owns one flat `faces` index array and a set of regions naming
// ranges in it; a batch pairs a region with a material. REGN is the geometry
// and BAT_ is the material binding, so this walks regions.
//
// Both were tried. Over 4507 corpus models the two selectors produce the
// identical mesh set — every region is named by a batch — so the measurement
// does not decide it and the choice rests on what each chunk means. Regions
// win on that: a batch cannot gate whether geometry exists when its only
// added information is a material index this phase does not read.
//
// ---------------------------------------------------------------------------
// A known gap, stated rather than hidden: REGN v2 has no face range
//
// WhiteoutLib's REGN parse visitor reads `firstIndex`/`indexCount` only for
// version >= 3; below that it reads `firstVertex`/`vertexCount` as u16 and
// stops. So a v2 region arrives with vertices and an index range of zero, and
// there is no way to reconstruct one here — a multi-region model gives no clue
// how its faces divide.
//
// Measured: 1471 of 4507 corpus models load vertices but draw nothing, and all
// 1471 are REGN v<3. It is the older SC2 and beta-era content; v3+ — which is
// the bulk of shipped SC2 and Heroes — is unaffected. Fixing it belongs in
// WhiteoutLib's parser, not here. Until then this warns per model rather than
// dropping silently.
//
// Region indices are region-local: `faces[region.firstIndex + i]` is already
// relative to `region.firstVertex`, which is why a u16 index array can address
// a model with more than 65535 vertices at all. That is asserted per index by
// the geometry test rather than assumed here — if it were global instead, every
// region past the first would index past its own vertex count and the test
// would name the model.
// ============================================================================

#include "io/m3/m3_animation.h"
#include "whiteout/flakes/content_ref.h"
#include "whiteout/flakes/model_source.h"

#include <whiteout/models/m3/m3.h>

#include <cstddef>
#include <memory>
#include <span>
#include <vector>

namespace whiteout::flakes::renderer::profiles::sc2_heroes {
struct Sc2ClothBuild;
}

namespace whiteout::flakes::io {

// ---------------------------------------------------------------------------
// The canonical texture set (M3_SIMPLE_MATERIAL_DESIGN.md §5)
//
// `.m3` has no texture list — layers name paths directly — so the renderer's
// texture ids have to come from a canonical enumeration. These helpers ARE
// that enumeration: `GetTextures()` emits exactly `CollectM3Textures()`, and
// `BuildM3SurfaceTable` indexes into the same call, so the two sides agree by
// construction rather than by parallel iteration code.
// ---------------------------------------------------------------------------

/// @brief The seven StandardMaterial layer slots the simple material system
///        consumes, in M3Surface order. Emissive2 is a real slot because the
///        shipped protoss set pairs a team-mask emissive1 (op TeamColor*Add)
///        with the actual glow in emissive2 — a shared slot can only carry
///        one of the two.
enum class M3LayerSlot : ::whiteout::u32 {
    Diffuse = 0,
    Decal,
    Specular,
    Emissive,
    Emissive2,
    Normal,
    AlphaMask,
    Count,
};

/// @brief One referenced texture, in canonical (first-seen, deduped) order.
struct M3TextureRef {
    std::string path; ///< NUL-trimmed, as authored (forward slashes).
    ::whiteout::u32 wrapFlags = 0; ///< Bit0 = repeat-U, bit1 = repeat-V.
};

/// @brief Strip the terminator every M3 `Ref<CHAR>` keeps. `size()` is one
///        past the text, so a raw copy carries an embedded NUL into the asset
///        key and misses CASC silently. Every path leaves through this.
std::string M3CleanPath(const std::string& raw);

/// @brief Whether @p layer samples a texture (a path, and not the solid-colour
///        flag 0x400) / contributes at all (texture or solid colour).
bool M3LayerHasTexture(const ::whiteout::m3::TextureLayer& layer);
bool M3LayerActive(const ::whiteout::m3::TextureLayer& layer);

/// @brief The layer serving @p slot, or null. AlphaMask falls back to its
///        second layer when the first is inactive (retail multiplies both;
///        one slot carries whichever exists). The emissive layers each have
///        their own slot — their blend ops differ per layer.
const ::whiteout::m3::TextureLayer* M3LayerForSlot(const ::whiteout::m3::StandardMaterial& mat,
                                                   M3LayerSlot slot);

/// @brief Every texture the standard materials reference through the seven
///        slots, deduped case-insensitively, first-seen order.
std::vector<M3TextureRef> CollectM3Textures(const ::whiteout::m3::Model& model);

/// @brief Geometry-only `IModelSource` over `whiteout::m3::Model`.
class M3ModelAdapter final : public ::whiteout::flakes::renderer::model::IModelSource {
public:
    /// @brief Parse the `.m3` in @p bytes. @p ref names the model in
    ///        diagnostics only — M3 resolves no siblings, so there is no
    ///        content provider to route through.
    ///
    /// Returns null when the parse throws or the model carries no drawable
    /// geometry.
    static std::shared_ptr<M3ModelAdapter> Load(const ContentRef& ref,
                                                std::span<const ::whiteout::u8> bytes);

    explicit M3ModelAdapter(::whiteout::m3::Model model);

    // ---- IModelDataSource ----
    std::vector<renderer::model::MeshData> GetMeshes() override;
    /// @brief One `TextureData` per `CollectM3Textures` entry: `sharedKey` =
    ///        the layer path, resolved through CASC by the asset manager. No
    ///        pixels travel through here.
    std::vector<renderer::model::TextureData> GetTextures() override;
    /// @brief Empty by design, M2's precedent: per-batch material data does
    ///        not fit `MaterialData` — the sc2_heroes profile builds
    ///        `M3SurfaceTable` straight off `SourceModel()`.
    std::vector<renderer::model::MaterialData> GetMaterials() override {
        return {};
    }
    /// @brief The BONE array as a node tree, with IREF as the inverse bind
    ///        pose.
    ///
    /// M3 is the first format here whose inverse bind matrices are real. MDX
    /// subtracts a pivot and M2 folds the pivot into the bone matrix, so both
    /// leave this identity; M3 ships the matrices in `IREF` and the palette is
    /// wrong without them.
    ///
    /// `nodePivots` stays empty on purpose — an M3 bone's animated TRS is a
    /// complete local transform with no separate pivot to compose around.
    renderer::model::SkeletonData GetSkeleton() override;

    /// @brief Per-region bone palettes. Emits **no** per-vertex data.
    ///
    /// The blob already holds `BoneWeights` and `BoneIndices` at offsets 12
    /// and 16, described by `DescribeM3Vertex` and uploaded untouched — the
    /// format ships them pre-coalesced so they can go straight to the GPU.
    /// Decoding them here would only feed a second vertex stream carrying the
    /// same numbers.
    ///
    /// What the renderer genuinely cannot infer is the palette: a vertex names
    /// its bone as an index into its region's window of `MODL.boneLookup`, so
    /// that window, resolved to global bone indices, *is* the geoset's palette.
    /// Filling `subsetNodeIndices` with it — and nothing else — is the whole
    /// job. See `SkinWeightData::paletteLocalVertexIndices`.
    std::vector<renderer::model::SkinWeightData> GetSkinWeights() override;

    /// @brief Raw `RegionFlag` bits per emitted geoset, parallel to
    ///        `GetMeshes()`.
    ///
    /// Carried so the cloth-simulated / cloth-influenced marks survive to the
    /// render side. Nothing consumes them yet; physics will, and recovering
    /// them later would mean re-walking the division.
    std::span<const ::whiteout::u32> GeosetRegionFlags() const {
        return geosetRegionFlags_;
    }
    std::vector<renderer::ParticleEmitterConfig> GetParticleConfigs() override {
        return {};
    }
    std::vector<renderer::effects::RibbonEmitterConfig> GetRibbonConfigs() override {
        return {};
    }
    /// @brief The `PHRB` rigid bodies as wireframes for the Collisions view.
    ///
    /// M3 has no chunk of plain collision primitives the way MDX has CLID, so
    /// this slot was empty and the physics bodies are exactly what it is for:
    /// they draw through the existing View > Collisions toggle with no new
    /// rendering path and no new UI, coloured by whether the solver is allowed
    /// to move them.
    ///
    /// It earns its place on a rig that misbehaves. A skinned mesh cannot
    /// separate "the bodies are in the wrong place" from "the bodies are right
    /// and the skinning is wrong"; drawing the bodies where the solver actually
    /// put them settles that in one look.
    ///
    /// Populated only when physics is compiled in; `physicsShapeBones_` records
    /// which bone each entry rides so `Evaluate` can place them.
    std::vector<renderer::model::CollisionShapeData> GetCollisionShapes() override;

    /// @brief `SDEV` keys, grouped into one config per (sequence, payload).
    ///
    /// M3 has no model-wide event array: an event *is* a key inside a
    /// sub-track container, carrying its own name, bone and option string. So
    /// every sequence's events are found by walking that sequence's STG group,
    /// and each config is stamped with `sequenceIndex` — without it an Attack
    /// container's cue fires during Walk, since every container's track starts
    /// at 0.
    ///
    /// The shipped vocabulary is three names (`Evt_Sound`, `Evt_SeqEnd`,
    /// `Evt_Simulate`) over the whole 3607-model corpus, and only `Evt_Sound`
    /// is an effect. See `M3DecodeEventKind`.
    std::vector<renderer::model::EventObjectConfig> GetEventObjects() override;

    /// @brief `MODL.bounds`, which M3 stores directly. Preferred over the
    ///        interface's union-over-positions default because it covers the
    ///        animated extent, and it is what camera framing measures against.
    ::whiteout::flakes::ModelBounds GetBounds() override;

    // ---- IAnimationSource ----
    /// @brief The `SEQS` table. Falls back to one synthetic sequence only when
    ///        the model has none at all, so the actor still has a clock.
    std::vector<renderer::model::SequenceInfo> GetSequences() const override;

    renderer::model::FrameState Evaluate(const PoseRequest& req) const override;

    /// @brief One `M3JtIkStage` per `IKJT` chain, then one `M3TurretStage` per
    ///        `PATU` behaviour.
    ///
    /// That order is the contract, not an accident of iteration: solvers run
    /// before physics, and within the solvers IK moves the mount a turret is
    /// bolted to. Appends nothing for a model with neither chunk, which is
    /// most of them.
    void CreatePoseStages(renderer::animation::PoseStageList& out) const override;

    /// @brief StarCraft II cross-fades between sequences rather than cutting,
    ///        on a 150 ms default its content is authored around.
    TransitionPolicy DefaultTransition() const override {
        TransitionPolicy p;
        p.crossFade = true;
        p.blendInMs = 150;
        p.blendOutMs = 150;
        return p;
    }

    /// @brief How many regions the chosen division holds. What the geometry
    ///        test asserts against.
    std::size_t RegionCount() const {
        return regionCount_;
    }

    /// @brief The regions `GetMeshes` emits, in emission order — `geosetId` is
    ///        an index into this. `BuildM3SurfaceTable` takes it so the table
    ///        never re-derives the skip filter.
    std::span<const std::size_t> EmittedRegions() const {
        return emittedRegions_;
    }

    const ::whiteout::m3::Model& SourceModel() const {
        return model_;
    }

private:
    /// @brief The regions `GetMeshes` emits, in emission order.
    ///
    /// `GetMeshes` skips empty and truncated regions, and `geosetId` is the
    /// index into what survives. Every other per-geoset accessor has to take
    /// exactly the same skips or the ids stop lining up, so the decision is
    /// made once, here, rather than repeated in each.
    void BuildEmittedRegions();

    /// @brief Expand a pose request into sampler layers, priority-desc.
    ///
    /// One clip becomes one layer per sub-track container in its sequence's
    /// group — the split-body expansion. Every layer of a clip carries that
    /// clip's index as its play id, which is what limits a clip to one
    /// contribution per property.
    void BuildLayers(const PoseRequest& req, std::vector<M3Layer>& out) const;

    /// @brief Sample one animatable property across @p layers.
    ///
    /// Walks the layers highest priority first, spending a weight budget from
    /// 1.0, then combines what it gathered lowest priority first with a
    /// smoothstep. Instantiated for the bone channels; the material and
    /// emitter channels reuse it unchanged when they land.
    template <typename T>
    T SampleRef(const ::whiteout::m3::AnimRef<T>& ref, std::span<const M3Layer> layers) const;

    /// @brief Sample a discrete channel in *override* mode.
    ///
    /// Separate from @ref SampleRef because the blend differs, not just the
    /// type: every contributor enters at a flat weight and the first past the
    /// filters wins outright. A visibility flag has no meaningful midpoint,
    /// and the engine routes these through its own override worker.
    ::whiteout::u32 SampleRefOverride(const ::whiteout::m3::AnimRef<::whiteout::u32>& ref,
                                      std::span<const M3Layer> layers) const;

    /// @brief Gate each emitted geoset on its region's root bone.
    void EvaluateGeosetVisibility(std::span<const ::whiteout::u8> visible,
                                  renderer::model::FrameState& fs) const;

    /// @brief Sample the `LITE` chunk into `FrameState::lights`.
    void EvaluateLights(std::span<const M3Layer> layers, std::span<const ::whiteout::u8> visible,
                        const Matrix44f& world, renderer::model::FrameState& fs) const;

    /// @brief Sample each `PHRB`'s `dynamicState` and place its shapes.
    ///
    /// Both halves are things only the source can do — one needs the layer
    /// stack, the other needs to agree with @ref GetCollisionShapes on shape
    /// order — and both feed the physics stage, which runs after this.
    void EvaluatePhysics(std::span<const M3Layer> layers,
                         renderer::model::FrameState& fs) const;

    ::whiteout::m3::Model model_;
    M3AnimTables tables_;
    // Which entry of `divisions` GetMeshes reads. Division 0 is the highest
    // detail level; the plan takes one LOD and no more.
    std::size_t divisionIndex_ = 0;
    std::size_t regionCount_ = 0;
    std::vector<std::size_t> emittedRegions_;
    std::vector<::whiteout::u32> geosetRegionFlags_;
    /// Which bone each entry of @ref GetCollisionShapes rides and the frame it
    /// sits in on that bone, parallel to the returned vector and filled by the
    /// same walk.
    std::vector<i32> physicsShapeBones_;
    std::vector<Matrix44f> physicsShapeLocals_;

    /// @brief The model's `PHCL` cloths, or null when it has none we can drive.
    ///
    /// Built once here rather than inside the stage because the *palette*
    /// depends on it: a cloth's particles are appended to the skeleton as
    /// nodes, so `GetSkeleton`, `GetSkinWeights` and `GetMeshes` all need the
    /// layout before any actor exists. Shared with every stage this adapter
    /// makes — the records are immutable, only the solver state is per actor.
    std::shared_ptr<const renderer::profiles::sc2_heroes::Sc2ClothBuild> cloth_;

    /// @brief Geoset -> the cloth piece whose particles skin it, `-1` for the
    ///        overwhelming majority that no cloth touches. Parallel to
    ///        @ref emittedRegions_.
    std::vector<i32> geosetClothPiece_;
    /// @brief Geoset -> "this is a cloth's invisible simulation proxy".
    std::vector<::whiteout::u8> geosetClothProxy_;

    /// @brief Fill @ref geosetClothPiece_ / @ref geosetClothProxy_ from
    ///        @ref cloth_. No-op without physics.
    void BuildClothGeosetMap();
    /// @brief Repoint one geoset's baked bone indices and weights at the cloth
    ///        particles that drive it (`PHAC`).
    void RewriteClothSkin(std::size_t geoset, renderer::model::MeshData& mesh) const;
};

} // namespace whiteout::flakes::io
