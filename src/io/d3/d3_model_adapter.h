#pragma once

// ============================================================================
// D3ModelAdapter — Diablo III actors and appearances.
//
// Named for the format, as the house rule requires; the *profile* is named for
// the game (Diablo3Profile). Sibling to MdxModelAdapter / M2ModelAdapter /
// M3ModelAdapter.
//
// ---------------------------------------------------------------------------
// D3 is the first format where the thing you load is not the thing you draw
//
// An `.acr` (Actor) is a gameplay object: bounds, a tag map, an AnimSet, and a
// weighted list of *look* names. An `.app` (Appearances) is the model: bones,
// geosets, materials, looks. `LoadActor` follows the first to the second and
// keeps the AnimSet; `LoadAppearance` opens one `.app`, uses look 0 and has no
// animations, which is what a model browser wants and what the offline gates
// use.
//
// **Neither calls native::loadActorModel.** That helper resolves snoAppearance
// / snoAnimSet / snoPhysics through a bare AssetProvider, which is a parse per
// reference with no sharing — and sharing is the norm here (594 actors name one
// appearance). So the adapter walks those references itself through D3SnoCache
// and gets the same shape with the parses deduplicated. The helper stays useful
// for a one-shot tool; it is not the renderer's path.
//
// ---------------------------------------------------------------------------
// The two bind poses, which is the highest-risk decision in the format
//
// A BoneStructure carries five PRSTransforms and three of the APP spec's five
// labels are wrong. Measured over Barbarian_Male's 47 bones to 1e-6:
//
//   tTransform0  model-space bind pose A          tTransform1  exact inverse of A
//   tTransform2  LOCAL parent-relative bind pose  tTransform3  model-space bind pose B
//   tTransform4  exact inverse of B
//
// A and B are *not the same pose*: 8.0% of bones over a 400-file sample have
// tTransform3 != tTransform0, deltas up to 19.9 units on a ~7-unit character,
// and 173 of 192 of them carry non-zero skin weight.
// Skeleton_BuildSkinningPaletteAndBounds reads bone+180, which is disk 0xEC —
// tTransform4. So:
//
//   inverseBindMatrices[i]  <-  tTransform4      // NOT tTransform1
//   localBindPose[i]        <-  tTransform2      // seeds an untracked bone
//   attachmentFrame[i]      <-  tTransform1      // hardpoints, trails, particles
//
// The mesh is authored in B's frame and the skeleton animates in A's, so the
// bind-pose palette is A·B-1 and is not identity for those 8%. Substituting
// tTransform1 makes 92% of every model correct and detonates the rest — which
// reads as "the exporter is broken" rather than as a wrong field.
//
// ---------------------------------------------------------------------------
// Rigid first
//
// 3,994 of 4,372 sampled sub-objects carry no vertex weights at all: they are
// positioned by SubObject.nBoneIndex. The rigid path is the common case and
// treating it as a fallback is backwards. Influence bone indices are *global*
// (21,701/21,701 sampled below boneCount) so there is no palette to remap, and
// weights already sum to 1.
//
// No 45-bone draw split. That is a constant-buffer artifact of the original —
// matBones is 135 float4 and the upload truncates past it — and our palette is
// not that buffer. D3's real ceiling is 512.
// ============================================================================

#include "io/d3/d3_sno_cache.h"
#include "io/d3/d3_types.h"
#include "whiteout/flakes/content_provider.h"
#include "whiteout/flakes/content_ref.h"
#include "whiteout/flakes/model_source.h"

#include <whiteout/sno/d3/native/d3_native.h>

#include <memory>
#include <span>
#include <string>
#include <vector>

namespace whiteout::flakes::io {

// ---------------------------------------------------------------------------
// The canonical texture set
//
// `.m3` had no texture list and needed CollectM3Textures to invent a canonical
// order. D3 has the same problem for the same reason — texture identity is a
// SNO id on a material entry, not an index into a model-level array — so it
// gets the same solution, and the same invariant: `GetTextures()` emits exactly
// this list and BuildD3SurfaceTable indexes into the same call, so the two
// agree by construction rather than by parallel iteration. That invariant broke
// twice on `.m3` before it was written down.
// ---------------------------------------------------------------------------

/// @brief Every texture the chosen look's materials reference, deduped by SNO
///        id, first-seen order.
std::vector<D3TextureRef> CollectD3Textures(const d3n::Appearances& app, u32 lookIndex);

/// @brief The same list, widened by whatever the per-geoset look overrides
///        reach.
///
/// @p lookByGeoset is parallel to @p emitted and may be shorter or empty; every
/// geoset it does not cover uses @p lookIndex. The uniform prefix is emitted
/// first and unchanged, so a model with no override produces a list identical
/// to the two-argument form — the canonical order is an index space the surface
/// table holds ids into, and permuting it silently rebinds every texture.
std::vector<D3TextureRef> CollectD3Textures(const d3n::Appearances& app, u32 lookIndex,
                                            std::span<const D3SubObjectRef> emitted,
                                            std::span<const u32> lookByGeoset);

/// @brief The material serving @p sub under @p lookIndex, or null.
///
/// `SubObject.szName` — *not* `szMaterialName`, whose name lies: it holds a
/// per-instance mesh id. Matched against `AppearanceMaterial.szName`
/// case-insensitively (the runtime compares 4-byte hashes; the disk holds the
/// strings), then `arVariants[lookIndex]` selects the per-look variant.
/// Measured: 2413 of 2413 corpus sub-objects join on szName, 1 on
/// szMaterialName.
const d3n::SubObjectAppearance* D3VariantFor(const d3n::Appearances& app, const d3n::SubObject& sub,
                                             u32 lookIndex);

/// @brief Which UberMaterial a variant actually draws with.
///
/// Where both are present the **embedded** one wins: it is the per-look
/// override and the SNO is the shared base. @p cache resolves the SNO half and
/// may be null, in which case only the embedded form is available.
const d3n::UberMaterial* D3MaterialOf(const d3n::SubObjectAppearance& variant, D3SnoCache* cache,
                                      std::shared_ptr<const d3n::Material>& keepAlive);

/// @brief The baked vertex record D3ModelAdapter::GetMeshes uploads.
///
/// 64 bytes: position, normal, both UV sets, tangent + bitangent sign, and both
/// vertex colours packed two-to-a-lane. Exposed so the geometry test can assert
/// the offsets rather than infer them from a render.
std::vector<renderer::model::VertexAttribute> DescribeD3Vertex();

/// @brief `IModelSource` over a parsed Diablo III appearance.
class D3ModelAdapter final : public ::whiteout::flakes::renderer::model::IModelSource {
public:
    /// @brief Open an `.acr`: follow snoAppearance, pick a look, keep the
    ///        AnimSet. @p bytes is the actor file the caller already read.
    static std::shared_ptr<D3ModelAdapter> LoadActor(const ContentRef& ref,
                                                     std::span<const u8> bytes, D3SnoCache& cache,
                                                     bool lazyClips = true);

    /// @brief Open an `.app` directly. Look 0, no animations.
    static std::shared_ptr<D3ModelAdapter> LoadAppearance(const ContentRef& ref,
                                                          std::span<const u8> bytes,
                                                          D3SnoCache& cache);

    D3ModelAdapter(std::shared_ptr<const d3n::Appearances> app, u32 lookIndex);

    // ---- IModelDataSource ----

    /// @brief One geoset per SubObject across tGeoSet0 then tGeoSet1.
    ///
    /// Parallel arrays, never MeshData::baked: `.m2` and `.m3` bake because
    /// their on-disk blob is already a valid vertex buffer, and D3's is not —
    /// every field but position needs unpacking, so there is no verbatim upload
    /// to preserve.
    std::vector<renderer::model::MeshData> GetMeshes() override;

    /// @brief One TextureData per CollectD3Textures entry.
    ///
    /// `sharedKey` is `"#<snoId>"` and **not** a scheme-prefixed name: the
    /// staging path parses that field with a fixed convention — a leading '#'
    /// means file id, anything else is a path — so any other spelling resolves
    /// against nothing and every texture sits on the white placeholder with no
    /// error anywhere.
    std::vector<renderer::model::TextureData> GetTextures() override;

    /// @brief Empty by design, M2's and M3's precedent: per-material data does
    ///        not fit MaterialData, and the profile builds D3SurfaceTable
    ///        straight off @ref SourceAppearance.
    std::vector<renderer::model::MaterialData> GetMaterials() override {
        return {};
    }

    /// @brief arBones as a node tree, with tTransform4 as the inverse bind.
    ///
    /// `nodePivots` stays empty — a PRSTransform is a complete local transform
    /// with no separate pivot to compose around, exactly as for `.m3`.
    renderer::model::SkeletonData GetSkeleton() override;

    /// @brief Per-geoset influences. Rigid sub-objects get one bone at weight
    ///        1; skinned ones expand three influences into four lanes.
    std::vector<renderer::model::SkinWeightData> GetSkinWeights() override;

    std::vector<renderer::ParticleEmitterConfig> GetParticleConfigs() override {
        return {};
    }
    std::vector<renderer::effects::RibbonEmitterConfig> GetRibbonConfigs() override {
        return {};
    }
    std::vector<renderer::model::CollisionShapeData> GetCollisionShapes() override {
        return {};
    }

    /// @brief Appearances::tBounds, which D3 ships directly. Preferred over the
    ///        interface's union-over-positions default.
    ::whiteout::flakes::ModelBounds GetBounds() override;

    // ---- IAnimationSource ----

    std::vector<renderer::model::SequenceInfo> GetSequences() const override;
    renderer::model::FrameState Evaluate(const PoseRequest& req) const override;

    /// @brief Read per clip from nBlendTicksFromOtherAnim, not a constant —
    ///        D3 is the second format after M3 to cross-fade and, unlike M3, it
    ///        states the ramp.
    TransitionPolicy DefaultTransition() const override;

    // ---- looks ------------------------------------------------------------
    //
    // The choice is the host's. The adapter defaults to the engine's weighted
    // pick with a *fixed* seed so a reload is reproducible; the renderer never
    // randomises behind the user. Census says the pick almost never has a
    // choice to make: weight 0 x 140,044 against 100 x 13,071.

    std::span<const std::string> Looks() const {
        return lookNames_;
    }
    u32 LookIndex() const {
        return lookIndex_;
    }
    /// @brief Choose a look. A look change rebuilds the surface table and
    ///        nothing else — the engine's own boundary
    ///        (ActorModel_BuildSubObjectRenderRecords runs once per look
    ///        change) — so the mesh upload survives.
    void SetLookIndex(u32 index);

    // ---- dressing ---------------------------------------------------------
    //
    // A player Appearance carries every armour variant at once — naked, light,
    // medium and heavy, for all four slots, plus the death and skill meshes —
    // and the game decides which of them draw. Nothing in the file says; the
    // rules live in ActorModel_ApplyLook. So the adapter holds the answer and
    // does not compute it: profiles::diablo3::D3CharacterAppearance is the only
    // caller, exactly as WowCharacterAppearance is for M2's geoset selection.

    /// @brief Which emitted geosets are held back. One byte per geoset, empty
    ///        for "all of them draw".
    ///
    /// Empty is the default and the only state a creature or a prop is ever in
    /// — the same reason M2's visible-geoset set defaults to everything rather
    /// than to nothing.
    void SetGeosetHidden(std::vector<u8> hidden) {
        geosetHidden_ = std::move(hidden);
    }
    std::span<const u8> GeosetHidden() const {
        return geosetHidden_;
    }

    /// @brief Per-geoset look override. Empty = every geoset resolves its
    ///        material at @ref LookIndex.
    ///
    /// The look is per *item* in the original — each equipped piece carries its
    /// own look name on tag 0x10401 — and one material serves a whole weight
    /// class, so a heavy chest and heavy boots from two different sets are one
    /// material read at two variant indices. A model-wide index cannot express
    /// that, which is why this is a vector and not a second scalar.
    void SetGeosetLooks(std::vector<u32> lookByGeoset) {
        geosetLooks_ = std::move(lookByGeoset);
    }
    std::span<const u32> GeosetLooks() const {
        return geosetLooks_;
    }
    /// @brief The look geoset @p g resolves under, override or not.
    u32 LookForGeoset(usize g) const {
        return (g < geosetLooks_.size()) ? geosetLooks_[g] : lookIndex_;
    }

    const d3n::Appearances& SourceAppearance() const {
        return *app_;
    }
    std::shared_ptr<const d3n::Appearances> SharedAppearance() const {
        return app_;
    }
    /// @brief The `.app`'s own SNO id. Half of the drawable cache key.
    i32 AppearanceSno() const {
        return app_ ? app_->dwSnoId : -1;
    }

    /// @brief The SubObjects GetMeshes emits, in emission order. `geosetId`
    ///        indexes into this, and the surface table takes it so the skip
    ///        filter is never derived twice.
    std::span<const D3SubObjectRef> EmittedSubObjects() const {
        return emitted_;
    }
    /// @brief The SubObject behind emitted geoset @p g, or null.
    const d3n::SubObject* SubObjectAt(usize g) const;

    /// @brief The frame hardpoints, trails and particle attachments read —
    ///        tTransform1, which is *not* what the palette inverts. Parallel to
    ///        the skeleton's nodes. Nothing consumes it yet; recovering it later
    ///        would mean re-walking the bones.
    std::span<const Matrix44f> AttachmentFrames() const {
        return attachmentFrames_;
    }

    /// @brief Attach the actor half: the AnimSet's clips and the sequence
    ///        table built from its core tag map.
    void BindAnimations(D3SnoCache& cache, std::shared_ptr<const d3n::AnimSet> animSet,
                        bool lazy);

private:
    /// @brief Decide which SubObjects are drawable, once, so every per-geoset
    ///        accessor takes the same skips and `geosetId` means one thing.
    void BuildEmittedSubObjects();
    void BuildSkeletonCache();

    /// @brief One playable clip: a tag, the Anim it names, and the permutation
    ///        chosen for it.
    struct Clip {
        i32 tagId = 0;
        i32 animSno = -1;
        std::string name;
        f32 durationSec = 0.0f;
        i32 blendInMs = 0;
        i32 blendOutMs = 0;
        /// @brief Parsed clip. Null until first play when lazy loading is on.
        mutable std::shared_ptr<const d3n::Anim> anim;
        /// @brief Which permutation plays. Re-picked on wrap under loop mode 2.
        mutable u32 permutation = 0;
        /// @brief Resolved once the Anim lands: permutation bone -> our node.
        mutable std::vector<i32> boneMap;
        mutable bool resolved = false;
    };

    /// @brief Fetch @p clip's Anim if it is not already in hand, and bind its
    ///        bones by name. Const because sampling is const and lazy loading
    ///        is the whole point; the members it fills are mutable.
    bool EnsureClip(const Clip& clip) const;

    std::shared_ptr<const d3n::Appearances> app_;
    std::shared_ptr<const d3n::AnimSet> animSet_;
    /// @brief Non-owning; the cache outlives every adapter it built (it is a
    ///        ModelLoader member and adapters die with their actors).
    D3SnoCache* cache_ = nullptr;
    bool lazyClips_ = true;

    u32 lookIndex_ = 0;
    std::vector<std::string> lookNames_;
    std::vector<D3SubObjectRef> emitted_;
    /// @brief Parallel to `emitted_`; both empty until something dresses this.
    std::vector<u8> geosetHidden_;
    std::vector<u32> geosetLooks_;

    /// @brief Composed once at build: the local bind pose (tTransform2) as a
    ///        matrix per bone, and the attachment frame (tTransform1).
    std::vector<Matrix44f> localBind_;
    std::vector<Matrix44f> attachmentFrames_;
    /// @brief tTransform2 as TRS, for seeding a bone no curve drives. Scale is
    ///        one float — a Vector3f would silently disagree with
    ///        Skeleton_ComposeWorldPose.
    struct BindTrs {
        Vector3f translation{0.0f, 0.0f, 0.0f};
        Quaternion rotation{};
        f32 scale = 1.0f;
    };
    std::vector<BindTrs> localBindTrs_;

    std::vector<Clip> clips_;
    std::vector<renderer::model::SequenceInfo> sequences_;
};

} // namespace whiteout::flakes::io
