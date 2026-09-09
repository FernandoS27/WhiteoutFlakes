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
#include <string>
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

/// @brief The StandardMaterial layer slots the simple material system
///        consumes, in M3Surface order. Emissive2 is a real slot because the
///        shipped protoss set pairs a team-mask emissive1 (op TeamColor*Add)
///        with the actual glow in emissive2 — a shared slot can only carry
///        one of the two. Appending only: a slot's ordinal is the texture id
///        `CollectM3Textures` hands the renderer.
enum class M3LayerSlot : ::whiteout::u32 {
    Diffuse = 0,
    Decal,
    Specular,
    Emissive,
    Emissive2,
    Normal,
    AlphaMask,
    /// SpecularExponent in retail's naming — modulates the Blinn exponent
    /// per pixel (psmaterial.fx:579 MaterialSpecularity).
    Gloss,
    /// Retail multiplies BOTH alpha layers into the coverage, so the second
    /// needs its own slot rather than standing in for a missing first.
    AlphaMask2,
    /// The reflection map (Envio). Sampled by direction rather than by UV, so
    /// unlike every slot above it it is a CUBE — the flat sphere maps behind
    /// the Spherical mappings are projected into one at decode time.
    Environment,
    /// EnvioMask — an ordinary UV-mapped 2D layer that scales the reflection.
    /// Its own slot because retail applies it and the env layer's own alpha
    /// together (psmaterial.fx:320-325), not one instead of the other.
    EnvironmentMask,
    Count,
};

/// @brief One referenced texture, in canonical (first-seen, deduped) order.
struct M3TextureRef {
    std::string path; ///< NUL-trimmed, as authored (forward slashes).
    ::whiteout::u32 wrapFlags = 0; ///< Bit0 = repeat-U, bit1 = repeat-V.
    /// Wanted as a cubemap. Part of the dedupe key, not a property of the
    /// path: one `.dds` can be an environment map on one material and a flat
    /// layer on another, and those are two different GPU textures.
    bool cube = false;
    /// Bound to the Normal slot, so it is linear data and must not be sampled
    /// through an sRGB view. In the dedupe key for the same reason `cube` is.
    ///
    /// The slot is the authority here because the filename is not: over the
    /// 51469-model SC2 + Heroes corpus, 16299 of 71792 normal-map references
    /// (22.7%) carry a name the path heuristic reads as colour
    /// (`Marine_Normal_Blood.dds`, `..._Normals.dds`, `Tank_Treads_Norms.dds`).
    /// See AssetManager's kTextureLinearSubKind for what that costs.
    bool linear = false;
};

/// @brief Strip the terminator every M3 `Ref<CHAR>` keeps. `size()` is one
///        past the text, so a raw copy carries an embedded NUL into the asset
///        key and misses CASC silently. Every path leaves through this.
std::string M3CleanPath(const std::string& raw);

/// @brief Whether @p layer samples a texture (a path, and not the solid-colour
///        flag 0x400) / contributes at all (texture or solid colour).
bool M3LayerHasTexture(const ::whiteout::m3::TextureLayer& layer);
bool M3LayerActive(const ::whiteout::m3::TextureLayer& layer);

/// @brief The layer serving @p slot, or null. One layer per slot — the
///        emissive and alpha-mask pairs each get their own, because their
///        blend ops differ and retail multiplies both alpha masks.
const ::whiteout::m3::TextureLayer* M3LayerForSlot(const ::whiteout::m3::StandardMaterial& mat,
                                                   M3LayerSlot slot);

/// @brief The `MAT_` / `CMP_` a `MATM` entry names, or null when it names
///        another type.
///
/// Neither hops: a composite is a stack of materials, and `EmittedRegions` is
/// already one geoset per section, so every MATM index a surface is built from
/// is a leaf by then.
const ::whiteout::m3::StandardMaterial*
M3StandardForMaterial(const ::whiteout::m3::Model& model, ::whiteout::u32 matmIndex);
const ::whiteout::m3::CompositeMaterial*
M3CompositeForMaterial(const ::whiteout::m3::Model& model, ::whiteout::u32 matmIndex);

/// @brief Every texture the standard materials reference through the
///        supported slots, deduped case-insensitively (and by cube-ness),
///        first-seen order.
std::vector<M3TextureRef> CollectM3Textures(const ::whiteout::m3::Model& model);

/// @brief `FrameState::texAnimMatrices` id for one standard material's layer
///        UV transform.
///
/// A pure function of the model, so the surface table and the evaluator agree
/// without sharing state: the table stamps this on every layer it resolves,
/// and the evaluator emits an entry only for the layers whose transform is not
/// the identity in every pose. A layer with no entry reads back as identity,
/// which is what 95% of them want — 134812 of the corpus's 2862196 standard
/// layers carry a live UV transform at all.
inline ::whiteout::i32 M3UvTransformId(::whiteout::u32 materialIndex, M3LayerSlot slot) {
    return static_cast<::whiteout::i32>(materialIndex * static_cast<::whiteout::u32>(
                                                            M3LayerSlot::Count) +
                                        static_cast<::whiteout::u32>(slot));
}

/// @brief Compose `p_m<L>UVTransform` — psmateriallayer.fx's 2x4, applied as
///        `mul(m, float4(u, v, 0, 1)).xy`.
///
/// TRS about the UV origin: tile, then spin in the UV plane, then offset.
/// `angle` is the LAYR's three-axis rotation and only `.z` (about W, i.e. in
/// the plane) can survive a 2x4 fed `(u, v, 0, 1)`; it is in radians —
/// measured, its shipped values cluster on ±pi/2 and ±pi. The rotation is
/// about the origin rather than about (0.5, 0.5) the way `.mdx`/`.m2` do
/// theirs: with wrap addressing the two agree for every quarter turn, which is
/// what shipped content authors, and origin-relative is what makes `uvTiling`
/// mean plain repeat count.
///
/// Only rows 0 and 1 exist because the third column and row can never reach
/// the output — the same reason `FrameState::TexAnimMatrix` stores two.
void M3ComposeUvTransform(const Vector2f& offset, const Vector3f& angle, const Vector2f& tiling,
                          ::whiteout::f32 row0[4], ::whiteout::f32 row1[4]);

/// @brief What `M3RestoreDataDrivenMaterials` made of a model's MADD records.
struct M3DataDrivenResult {
    ::whiteout::u32 restored = 0;     ///< Rebuilt from a fixed-function record.
    ::whiteout::u32 approximated = 0; ///< Inferred from a shader graph.
    ::whiteout::u32 refused = 0;      ///< No standard form; the surface stays unlit and undrawn.
};

/// @brief Rewrite every `MaterialType::DataDriven` map into a StandardMaterial.
///
/// MADD is not a kind of material — it is what the engine converts every MAT_,
/// DIS_ and REF_ into at load, and Heroes of the Storm ships models already in
/// that form: 686 across the corpus, and in all of them *every* MATM entry is
/// data-driven, 2581 of 2661 with not one MAT_ beside them. Nothing downstream
/// reads MADD, so those 582 drawable models came out with no material at all.
///
/// The conversion has an inverse, so run it: WhiteoutLib reverses it exactly
/// where the record is fixed-function (86%) and infers a likeness from node
/// types and texture names where it is a graph. The result is appended to
/// `standardMaterials` and the map repointed at it, so texture collection, the
/// surface table and composite resolution all see an ordinary standard material
/// and need no MADD path of their own. A record with no standard form — a
/// converted DIS_ or REF_, or a graph with no assignable texture role — keeps
/// its data-driven map and stays undrawn, which is what it did before.
M3DataDrivenResult M3RestoreDataDrivenMaterials(::whiteout::m3::Model& model);

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
    /// @brief One config per `RIB_` record, in file order — the emitter id IS
    ///        the record index. Field mapping notes live at the definition;
    ///        the big one: WhiteoutLib's `emitterShape`/`ribbonType` carry the
    ///        pre-RE labels, so they land here as `ribbonType` (cross-section)
    ///        and `cullMethod` (SC2_RIBBON_RE.md §1.1).
    std::vector<renderer::effects::Sc2RibbonEmitterConfig> GetSc2RibbonConfigs() override;
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
    std::vector<renderer::model::ClothOverlayData> GetClothOverlays() override;

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
    ///
    /// A region repeats once per section of a composite material, which is what
    /// makes `EmittedMaterials` rather than the region's batch the answer to
    /// "which material does this geoset draw".
    std::span<const std::size_t> EmittedRegions() const {
        return emittedRegions_;
    }

    /// @brief The `MATM` index each emitted geoset draws, parallel to
    ///        @ref EmittedRegions.
    std::span<const ::whiteout::u32> EmittedMaterials() const {
        return emittedMaterials_;
    }

    const ::whiteout::m3::Model& SourceModel() const {
        return model_;
    }

    // ---- external animation files (`.m3a`) --------------------------------
    //
    // StarCraft II never finds these from the `.m3` — the chunk table holds no
    // path of any kind. The game reads them off the model's catalog entry
    // (`CModel.RequiredAnims` / `RequiredAnimsEx`) and merges each one into a
    // single global sequence and container index space, binding tracks to the
    // model purely by `animId`. We have no catalog, so the host names the file;
    // everything downstream of that is the shipped mechanism.

    /// @brief One attached animation file, as reported to a host.
    struct AttachedAnimation {
        /// @brief What the host called it — a file stem, shown in the UI and
        ///        used to reject a double attach.
        std::string label;
        /// @brief Sequences this file contributed.
        std::size_t sequenceCount = 0;
        /// @brief Where its sequences start in `GetSequences()`.
        std::size_t firstSequence = 0;
    };

    /// @brief Parse @p bytes as a `.m3a` and merge its sequences into this
    ///        model's.
    ///
    /// Sequence indices already handed out stay valid: the new sequences append
    /// after every existing one. Callers must re-read @ref GetSequences
    /// afterwards (`AnimationDriver::Bind` does).
    ///
    /// Returns false when the parse fails, when the file carries no sequences,
    /// or when @p label is already attached.
    bool AttachAnimationFile(std::string label, std::span<const ::whiteout::u8> bytes);

    /// @brief Drop one attached file. **Renumbers** every sequence after it.
    bool DetachAnimationFile(std::size_t index);

    /// @brief Drop them all, leaving only the model's own sequences.
    void ClearAnimationFiles();

    std::span<const AttachedAnimation> AttachedAnimations() const {
        return attached_;
    }

    /// @brief The parsed file behind @ref AttachedAnimations()[@p index], or
    ///        null past the end.
    ///
    /// Which `.m3a` belongs to a model is host state — the `.m3` names none of
    /// them — so anything that has to reproduce what is on screen, an exporter
    /// included, has to be handed the files. @ref tables_ is no use to one: it
    /// is indexed for playback, and a converter needs the sequences as the file
    /// holds them.
    const ::whiteout::m3::Model* AttachedAnimationModel(std::size_t index) const {
        return index < animModels_.size() ? animModels_[index].get() : nullptr;
    }

    // ---- sub-tracks ------------------------------------------------------
    //
    // A `SEQS` entry is a name and a window; the keys live in the `STC_`
    // containers its `STG_` group lists, and there is usually more than one.
    // The Marine's `Cover` is `Cover_full` plus `Cover_Shield`; its
    // `Stand Right Ready` is `_full` plus `_Legs2`. Playing the sequence runs
    // every container, which is what the game does; naming one is how a host
    // borrows a single prop or limb out of a sequence it does not otherwise
    // want. See `ClipRef::subtrack`.

    /// @brief One sub-track container of a sequence.
    struct SubtrackInfo {
        /// @brief The container name with the sequence's own name and the
        ///        separating `_` trimmed off — `Cover_Shield` reads `Shield`.
        ///        Falls back to the full container name when it does not carry
        ///        the prefix, and to `full` when the container is unnamed.
        std::string name;
        /// @brief Priority the container blends at, highest first.
        ::whiteout::u16 priority = 0;
        /// @brief `STC.runsConcurrent`: the container abstains on properties it
        ///        has no track for, instead of forcing their defaults.
        bool concurrent = false;
        /// @brief How many `animId`s this container drives. A one-property
        ///        container is a switch, not a pose.
        std::size_t trackCount = 0;
    };

    /// @brief The containers @p sequence spans, in the order `ClipRef::subtrack`
    ///        indexes them. Empty for an out-of-range index.
    std::vector<SubtrackInfo> SubtracksOf(::whiteout::i32 sequence) const;

private:
    /// @brief Does @p sequence play by itself, forever, over everything else?
    ///        Backs `SequenceInfo::alwaysPlays`.
    bool IsGlobalLoop(::whiteout::i32 sequence) const;

    /// @brief The regions `GetMeshes` emits, in emission order.
    ///
    /// `GetMeshes` skips empty and truncated regions, and `geosetId` is the
    /// index into what survives. Every other per-geoset accessor has to take
    /// exactly the same skips or the ids stop lining up, so the decision is
    /// made once, here, rather than repeated in each.
    void BuildEmittedRegions();

    /// @brief Invert the `BBSC` list into @ref boneBillboard_.
    void BuildBoneBillboards();

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

    /// @brief Gate each emitted geoset on its batch's visibility bone
    ///        (@ref geosetVisibilityBone_) and fill its alpha from the
    ///        composite-section multiplier (@ref emittedWeights_).
    void EvaluateGeosetVisibility(std::span<const M3Layer> layers,
                                  std::span<const ::whiteout::u8> visible,
                                  renderer::model::FrameState& fs) const;

    /// @brief Sample the `LITE` chunk into `FrameState::lights`.
    void EvaluateLights(std::span<const M3Layer> layers, std::span<const ::whiteout::u8> visible,
                        const Matrix44f& world, renderer::model::FrameState& fs) const;

    /// @brief Sample every `RIB_`'s animated tracks into
    ///        `FrameState::ribbonStates` (the `sc2` block).
    void EvaluateRibbons(std::span<const M3Layer> layers,
                         std::span<const ::whiteout::u8> visible, const Matrix44f& world,
                         renderer::model::FrameState& fs) const;

    /// @brief Sample every standard material layer's UV transform into
    ///        `FrameState::texAnimMatrices`.
    ///
    /// Emits only the layers whose transform actually moves — a driven track,
    /// or a bind-pose value that is not the identity. The rest are absent, and
    /// `M3UvTransformId` is what lets the surface table name a slot it never
    /// has to look up.
    void EvaluateMaterialUvTransforms(std::span<const M3Layer> layers,
                                      renderer::model::FrameState& fs) const;

    /// @brief Sample each `PHRB`'s `dynamicState` and place its shapes.
    ///
    /// Both halves are things only the source can do — one needs the layer
    /// stack, the other needs to agree with @ref GetCollisionShapes on shape
    /// order — and both feed the physics stage, which runs after this.
    void EvaluatePhysics(std::span<const M3Layer> layers,
                         renderer::model::FrameState& fs) const;

    /// @brief Rebuild @ref tables_ over the model plus @ref animModels_.
    ///
    /// Every attach and detach rebuilds from scratch, exactly as StarCraft II
    /// does (`sub_1028F3580` re-runs the whole merge over every loaded record
    /// on each add). Incremental merging would have to redo the animId row
    /// allocation anyway, and this runs once per user action.
    void RebuildAnimationTables();

    ::whiteout::m3::Model model_;
    /// @brief Attached `.m3a` models. Held behind `unique_ptr` because
    ///        @ref tables_ keeps pointers into them and this vector grows.
    std::vector<std::unique_ptr<::whiteout::m3::Model>> animModels_;
    /// @brief Host-facing description of @ref animModels_, same order.
    std::vector<AttachedAnimation> attached_;
    M3AnimTables tables_;
    // Which entry of `divisions` GetMeshes reads. Division 0 is the highest
    // detail level; the plan takes one LOD and no more.
    std::size_t divisionIndex_ = 0;
    std::size_t regionCount_ = 0;
    std::vector<std::size_t> emittedRegions_;
    /// @brief Geoset -> the `MATM` index it draws. Parallel to
    ///        @ref emittedRegions_; a composite contributes one entry per
    ///        section, all naming the same region.
    std::vector<::whiteout::u32> emittedMaterials_;
    /// @brief Geoset -> the composite section's animated multiplier, or a
    ///        constant one for a geoset that is not a composite pass. Sampled
    ///        into `FrameState::geosetAlphas`, which is where retail's
    ///        `AlphaFactor` (psmaterial.fx:358) sits: before the alpha-mask
    ///        layers and before the test.
    std::vector<::whiteout::m3::AnimRef<f32>> emittedWeights_;
    std::vector<::whiteout::u32> geosetRegionFlags_;
    /// @brief Geoset -> the bone whose visibility gates its draw, `0xFFFF` for
    ///        "always drawn". Parallel to @ref emittedRegions_.
    ///
    /// It is the BATCH's bone (`BAT_`'s last u16, the field WhiteoutLib names
    /// `boneCount`), not the region's `rootBone`: StarCraft II's submit loop
    /// (`sub_10290A190`) reads the batch record's bone and skips the batch
    /// when that bone's runtime visible bit is clear. Measured over the SC2
    /// and HotS corpora, every one of the 1967 batches that names a bone names
    /// one with a keyed visibility track, and only 422 of them coincide with
    /// the region's root bone — the Ultralisk's blood plane is a batch gated
    /// on `Plane01` inside a region rooted at `Dummy09`.
    std::vector<::whiteout::u16> geosetVisibilityBone_;
    /// @brief Bone -> index into `model_.billboardBehaviors`, or -1.
    ///
    /// Empty when the model has no `BBSC` chunk, which is the test `Evaluate`
    /// uses to skip the camera work entirely — 90% of the corpus. Two records
    /// naming the same bone leaves the later one, matching the engine, whose
    /// solvers run in array order and each overwrite the bone's local rotation.
    std::vector<i32> boneBillboard_;
    /// Which bone each entry of @ref GetCollisionShapes rides, the frame it sits
    /// in on that bone, and whether it may wear the bone's scale per axis — all
    /// parallel to the returned vector and filled by the same walk.
    std::vector<i32> physicsShapeBones_;
    std::vector<Matrix44f> physicsShapeLocals_;
    std::vector<::whiteout::u8> physicsShapeAniso_;

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
