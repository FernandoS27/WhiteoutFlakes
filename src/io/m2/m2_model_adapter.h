#pragma once

// ============================================================================
// M2ModelAdapter — World of Warcraft `.m2`, geometry only.
//
// Positions and indices from the chosen skin profile's submeshes, and nothing
// else: no bones, no textures, no materials, no batches beyond "which
// submesh". Every surface is drawn by UnlitShading, so "the model loads" is a
// visible, gate-able claim before any of the material work exists
// (REFACTOR_PLAN.md P9).
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

#include "whiteout/flakes/content_provider.h"
#include "whiteout/flakes/model_source.h"

#include <whiteout/interfaces.h>
#include <whiteout/models/m2/m2.h>

#include <optional>
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
    static std::shared_ptr<M2ModelAdapter> Load(const ContentRef& ref,
                                                std::span<const ::whiteout::u8> bytes,
                                                IContentProvider* provider);

    explicit M2ModelAdapter(::whiteout::m2::Model model);

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

    /// @brief The model's own bounding box, which `.m2` stores directly —
    ///        better than the default's union over mesh positions because it
    ///        covers the animated extent, and it is what camera framing needs
    ///        to not render a 2-yard creature as a sub-pixel dot.
    ::whiteout::flakes::ModelBounds GetBounds() override;

    // ---- IAnimationSource ----
    /// @brief One placeholder sequence. Geometry-only: there are no bone
    ///        tracks to sample, so every pose is the bind pose and the actor
    ///        still needs a sequence to keep its clock running.
    std::vector<renderer::model::SequenceInfo> GetSequences() const override;

    renderer::model::FrameState Evaluate(const PoseRequest& req) const override;

    /// @brief How many submeshes the chosen profile contributed. What the
    ///        geometry test asserts against.
    std::size_t SubmeshCount() const {
        return submeshCount_;
    }

    const ::whiteout::m2::Model& SourceModel() const {
        return model_;
    }

private:
    ::whiteout::m2::Model model_;
    // Which entry of `skinProfiles` GetMeshes reads. Index 0 is the highest
    // detail level; the plan takes one LOD and no more.
    std::size_t profileIndex_ = 0;
    std::size_t submeshCount_ = 0;
};

} // namespace whiteout::flakes::io
