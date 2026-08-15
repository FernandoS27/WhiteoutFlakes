#pragma once

// ============================================================================
// M2ModelAdapter — World of Warcraft `.m2`.
//
// Geometry and textures from the chosen skin profile; per-batch material data
// stays out of `MaterialData` and is built into `M2SurfaceTable` instead (see
// below). Animation is here: the bone hierarchy, the per-sequence track
// sampling, texture transforms, and the animated half of each batch's colour /
// alpha / per-unit weights.
//
// ---------------------------------------------------------------------------
// How the `.skin` arrives — the load route, chosen and stated
//
// The plan offered two: resolve the bundle through AssetManager's needs queue
// and re-Build() when the skin lands, or route the whole thing through
// SpawnUnitFromSource. It also said to pick one and delete the other, because
// the design's justification for the first rested on a claim about
// ModelTemplateManager that turned out to be false.
//
// This picks the second, and for a reason that only became visible once the
// parser was read: `m2::Parser::parse` takes an `interfaces::CascFileSystem`
// and resolves the `.skin` (and `.skel`) siblings *itself*, synchronously, by
// fileDataID. A needs-queue bundle would have to fight that — either
// re-entering the parser once per sibling or reimplementing chunk resolution
// outside it. Handing the parser a CascFileSystem backed by IContentProvider
// is the whole of the work, and it is exactly the id-addressed route P6 built.
//
// The cost is that loading blocks on IO. That is not new: MDX's
// ModelTemplateManager::GetOrLoadSync already does Request+Wait on the same
// thread, so this matches the existing load route rather than inventing a
// second one.
//
// ---------------------------------------------------------------------------
// Two sibling-resolution routes, because there are genuinely two
//
// The parser offers `parse(CascFileSystem&, bytes)` and
// `parse(VirtualPathFileSystem&, path)`, and which one applies is decided by
// how the model was named — the same discriminant ContentRef already carries:
//
//   FileId → CascFileSystem.        Siblings referenced by fileDataID. This is
//                                   how a shipped WoW install stores chunked
//                                   (Legion+) models, and it needs nothing but
//                                   the id route P6 built.
//   Path   → VirtualPathFileSystem. Siblings are files on disk next to the
//                                   model: `foo.m2` + `foo00.skin`. This is
//                                   how extracted corpora are laid out, and it
//                                   is the route the gate runs on.
//
// An earlier pass here implemented only the CASC one, on the assumption that
// id-addressing was the whole story. It is not: a fileDataID cannot be
// resolved out of a shipped install without a listfile, so the corpus — and
// therefore every test and every golden — is path-addressed. Both routes are
// small; having only one made the format untestable.
// ============================================================================

#include "io/m2/m2_animation.h"
#include "whiteout/flakes/content_provider.h"
#include "whiteout/flakes/model_source.h"

#include <whiteout/interfaces.h>
#include <whiteout/models/m2/m2.h>

#include <optional>
#include <utility>
#include <vector>

namespace whiteout::flakes::io {

/// @brief `interfaces::CascFileSystem` over an `IContentProvider`.
///
/// The adapter between WhiteoutLib's id-addressed model parsers and this
/// renderer's content abstraction. Every read is a `ContentRef::FromFileId`,
/// which the desktop provider resolves through `casc::Storage::readFile(i32)`.
class ContentProviderCascFs final : public ::whiteout::interfaces::CascFileSystem {
public:
    explicit ContentProviderCascFs(IContentProvider* provider) : provider_(provider) {}

    std::vector<::whiteout::u8> readFile(::whiteout::u32 fileId) const override;

    /// @brief Path → fileDataID. Unsupported: resolving one needs a listfile
    ///        or a root-manifest name lookup, neither of which the content
    ///        provider exposes. M2 references its siblings by id, so nothing
    ///        in this phase asks.
    std::optional<::whiteout::u32> reserveFileId(const std::string& path) override {
        (void)path;
        return std::nullopt;
    }

    /// @brief Read-only. The renderer never writes into game storage.
    bool writeFile(::whiteout::u32 fileId, const std::vector<::whiteout::u8>& data) override {
        (void)fileId;
        (void)data;
        return false;
    }

    bool fileExists(::whiteout::u32 fileId) const override;

private:
    IContentProvider* provider_ = nullptr;
};

/// @brief `interfaces::VirtualPathFileSystem` over an `IContentProvider`.
///
/// The path counterpart of ContentProviderCascFs, for models whose `.skin` and
/// `.anim` siblings sit next to them on disk rather than behind fileDataIDs.
class ContentProviderPathFs final : public ::whiteout::interfaces::VirtualPathFileSystem {
public:
    explicit ContentProviderPathFs(IContentProvider* provider) : provider_(provider) {}

    std::vector<::whiteout::u8> readFile(const std::string& path) const override;
    bool fileExists(const std::string& path) const override;

    /// @brief Read-only, like its CASC twin.
    bool writeFile(const std::string& path, const std::vector<::whiteout::u8>& data) override {
        (void)path;
        (void)data;
        return false;
    }

    /// @brief Not needed: the parser resolves siblings by constructing their
    ///        names from the model's, never by listing a directory.
    std::vector<::whiteout::interfaces::DirectoryEntry>
    listDirectory(const std::string& path) const override {
        (void)path;
        return {};
    }

private:
    IContentProvider* provider_ = nullptr;
};

/// @brief Geometry-only `IModelSource` over `whiteout::m2::Model`.
class M2ModelAdapter final : public ::whiteout::flakes::renderer::model::IModelSource {
public:
    /// @brief Parse the `.m2` @p ref names, resolving its `.skin` siblings
    ///        through @p provider by whichever route @p ref's discriminant
    ///        selects. @p bytes is the already-read model, so the caller's
    ///        magic sniff is not repeated.
    ///
    /// Returns null when the parse fails or no skin profile resolved — the
    /// latter meaning the siblings could not be found, which is a
    /// configuration problem rather than a malformed model.
    ///
    /// With @p lazyAnimations, the parse skips the `.anim` siblings and
    /// Evaluate reads one the first time a sequence is played. The adapter
    /// keeps the filesystem wrapper alive for that, which is why @p provider
    /// then has to outlive the adapter — as it already does, being the scene's.
    static std::shared_ptr<M2ModelAdapter> Load(const ContentRef& ref,
                                                std::span<const ::whiteout::u8> bytes,
                                                IContentProvider* provider,
                                                bool lazyAnimations = false);

    /// @p fsKeepAlive is the filesystem wrapper the model was parsed through.
    /// A lazily parsed model reads its `.anim` siblings through it for the life
    /// of the adapter, so the adapter has to own it; the exact type does not
    /// matter here, only that it stays alive. Null for an eager parse.
    explicit M2ModelAdapter(::whiteout::m2::Model model,
                            std::shared_ptr<void> fsKeepAlive = nullptr);

    // ---- IModelDataSource ----
    std::vector<renderer::model::MeshData> GetMeshes() override;

    /// @brief One entry per M2 texture, keyed by its index in the model's
    ///        texture list — which is also what the batch combo tables resolve
    ///        to, so a resolved combo value *is* a renderer texture id.
    ///
    /// Nothing is decoded here: a named texture ships only its `sharedKey` and
    /// the AssetManager fetches it, so a model with 30 textures does not block
    /// the load thread 30 times. Textures with `type != 0` name no file of
    /// their own — the game fills them — so they bind whatever
    /// SetReplaceableTextures was given, and the white default otherwise.
    std::vector<renderer::model::TextureData> GetTextures() override;

    /// @brief What fills the model's replaceable slots, indexed by
    ///        `M2Texture::type`. Each entry is a texture key — `#<fileDataID>`
    ///        or a path. An empty entry, or a type past the end, keeps the
    ///        white default.
    ///
    /// Set before Build(). The renderer resolves this per spawn rather than per
    /// model — see profiles::wow::WowReplaceableTextures, which picks the file
    /// the same way the client does.
    void SetReplaceableTextures(std::vector<std::string> byTextureType) {
        replaceableByType_ = std::move(byTextureType);
    }

    /// @brief Empty by design. M2's per-batch material data does not fit
    ///        `MaterialData` — see M2SurfaceTable, which the WoW profile builds
    ///        straight off `SourceModel()`.
    std::vector<renderer::model::MaterialData> GetMaterials() override {
        return {};
    }
    /// @brief The bone list, flat, in file order — which is also topological
    ///        order, since the client asserts `parentIndex < boneIndex`.
    ///
    /// `inverseBindMatrices` are all identity, and that is not a stub: an M2
    /// bone matrix maps model space to *animated* model space because the pivot
    /// is folded into it (`T(-pivot) · S · R · T(pivot + t)`), so there is no
    /// separate bind-pose inverse to undo. `CM2Model::GetBonePositionByIndex`
    /// confirms it — it reads the bone's pivot straight through the bone matrix
    /// with nothing in between.
    renderer::model::SkeletonData GetSkeleton() override;

    /// @brief One entry per submesh, weights from the `.m2` vertex record.
    ///
    /// Bone indices are rebased onto a per-submesh subset rather than shipped
    /// global. The renderer's palette layout wants that shape (it is what
    /// `subsetNodeIndices` means), and it is also what keeps a rig with more
    /// than 256 bones drawable: the per-actor palette is capped at 256 slots
    /// and only a per-geoset subset fits under it.
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

    /// @brief The model's own bounding box, which `.m2` stores directly —
    ///        better than the default's union over mesh positions because it
    ///        covers the animated extent, and it is what camera framing needs
    ///        to not render a 2-yard creature as a sub-pixel dot.
    ::whiteout::flakes::ModelBounds GetBounds() override;

    /// @brief `globalLoops`, the periods every global-sequence track runs on.
    std::vector<u32> GetGlobalSequences() override;

    // ---- IAnimationSource ----
    /// @brief One entry per `M2Sequence`, named from the client's
    ///        `AnimationData` table and timed `[0, duration)`.
    ///
    /// Aliases are kept rather than resolved: an alias entry is a real index a
    /// host can select, and the track sampler already falls back to sub-array 0
    /// for a sequence with no keys of its own — which is what an alias is.
    std::vector<renderer::model::SequenceInfo> GetSequences() const override;

    /// @brief Sample the whole model at one pose: bone matrices, texture
    ///        transforms, and per-batch colour / alpha / weights.
    ///
    /// On a lazily parsed model this is also where a sequence's `.anim` sibling
    /// gets read — the first frame that plays it, and no earlier. Same place
    /// the client does it: CM2Model::LoadSequence runs off SetBoneSequence, not
    /// off the model load.
    renderer::model::FrameState Evaluate(const PoseRequest& req) const override;

    /// @brief How many submeshes the chosen profile contributed. What the
    ///        geometry test asserts against.
    std::size_t SubmeshCount() const {
        return submeshCount_;
    }

    const ::whiteout::m2::Model& SourceModel() const {
        return model_;
    }

    /// @brief Which `skinProfiles` entry GetMeshes read. The surface table has
    ///        to be built against the same one — batches are per profile.
    std::size_t ProfileIndex() const {
        return profileIndex_;
    }

private:
    void EvaluateBones(const M2AnimTime& at, bool bindPose,
                       renderer::model::FrameState& fs) const;
    void EvaluateTextureTransforms(const M2AnimTime& at, bool bindPose,
                                   renderer::model::FrameState& fs) const;
    void EvaluateSurfaces(const M2AnimTime& at, bool bindPose,
                          renderer::model::FrameState& fs) const;

    // Mutable because Evaluate is const and a lazily parsed model fills its
    // tracks in on first play. Nothing a caller can observe changes: the keys
    // that arrive are the ones the eager parse would already have read.
    mutable ::whiteout::m2::Model model_;
    // The parse-time filesystem wrapper, held only for a lazy parse. See the
    // constructor.
    std::shared_ptr<void> fsKeepAlive_;
    // Indexed by M2Texture::type; empty until the WoW profile resolves them.
    std::vector<std::string> replaceableByType_;
    // Which entry of `skinProfiles` GetMeshes reads. Index 0 is the highest
    // detail level; the plan takes one LOD and no more.
    std::size_t profileIndex_ = 0;
    std::size_t submeshCount_ = 0;
    // `globalLoops` flattened at construction. Evaluate is const and needs the
    // periods every frame; GetGlobalSequences hands the same list to the loader.
    std::vector<u32> globalLoops_;
};

} // namespace whiteout::flakes::io
