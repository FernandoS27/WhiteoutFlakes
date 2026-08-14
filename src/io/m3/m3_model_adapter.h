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

#include "whiteout/flakes/content_ref.h"
#include "whiteout/flakes/model_source.h"

#include <whiteout/models/m3/m3.h>

#include <memory>
#include <span>
#include <vector>

namespace whiteout::flakes::io {

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
    std::vector<renderer::model::TextureData> GetTextures() override {
        return {};
    }
    std::vector<renderer::model::MaterialData> GetMaterials() override {
        return {};
    }
    renderer::model::SkeletonData GetSkeleton() override {
        return {};
    }
    std::vector<renderer::model::SkinWeightData> GetSkinWeights() override {
        return {};
    }
    std::vector<renderer::ParticleEmitterConfig> GetParticleConfigs() override {
        return {};
    }
    std::vector<renderer::effects::RibbonEmitterConfig> GetRibbonConfigs() override {
        return {};
    }
    std::vector<renderer::model::CollisionShapeData> GetCollisionShapes() override {
        return {};
    }

    /// @brief `MODL.bounds`, which M3 stores directly. Preferred over the
    ///        interface's union-over-positions default because it covers the
    ///        animated extent, and it is what camera framing measures against.
    ::whiteout::flakes::ModelBounds GetBounds() override;

    // ---- IAnimationSource ----
    /// @brief One placeholder sequence, as for `.m2`. Geometry-only: there are
    ///        no bone tracks to sample, so every pose is the bind pose, and the
    ///        actor still needs a sequence to keep its clock running.
    std::vector<renderer::model::SequenceInfo> GetSequences() const override;

    renderer::model::FrameState Evaluate(const PoseRequest& req) const override;

    /// @brief How many regions the chosen division holds. What the geometry
    ///        test asserts against.
    std::size_t RegionCount() const {
        return regionCount_;
    }

    const ::whiteout::m3::Model& SourceModel() const {
        return model_;
    }

private:
    ::whiteout::m3::Model model_;
    // Which entry of `divisions` GetMeshes reads. Division 0 is the highest
    // detail level; the plan takes one LOD and no more.
    std::size_t divisionIndex_ = 0;
    std::size_t regionCount_ = 0;
};

} // namespace whiteout::flakes::io
