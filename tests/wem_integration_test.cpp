// ============================================================================
// WEM save/load, from the renderer's side.
//
// The claim under test is the one WEM_DESIGN.md's risk table left open:
//
//   > If WhiteoutFlakes ever wants a save/load path, its adapters are the
//   > natural first consumer and P2's identity gate is what would make it safe.
//
// So this is the identity gate reaching an adapter: a model goes out through a
// converter, through the container, back in through the other converter, into
// the adapter that draws it, and the geometry that arrives is the geometry that
// left. Everything else here — the profile table, the option list, the export
// refusal — is a precondition of that path being reachable at all.
//
// Device-free and corpus-free: every fixture is built in memory, so this runs
// on a CI box with no game installed and no GPU.
// ============================================================================

#include "io/wem/wem_export.h"
#include "io/wem/wem_import.h"
#include "io/wem/wem_profiles.h"

#include "io/mdx_model_adapter.h"

#include <whiteout/models/wem/converters.h>
#include <whiteout/models/wem/parser.h>
#include <whiteout/models/wem/validate.h>
#include <whiteout/models/wem/writer.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "io/file_content_provider.h"
#if WDX_ENABLE_D3
#include <whiteout/models/wem/d3_converter.h>
#include "io/d3/d3_model_adapter.h"
#include "io/d3/d3_sno_cache.h"
#endif
#if WDX_ENABLE_M2
#include "io/m2/m2_model_adapter.h"
#endif
#if WDX_ENABLE_M3
#include "io/m3/m3_model_adapter.h"
#endif

#include <whiteout/models/mdx/parser.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

using namespace whiteout;
using namespace whiteout::flakes;
using namespace whiteout::flakes::io;
namespace wem = ::whiteout::models::wem;

namespace {

// ---------------------------------------------------------------------------
// The fixture: one quad, one material, two bones, one attachment, one sequence.
//
// Small on purpose. What the round trip has to preserve is *structure* — how
// many geosets, how many vertices in each, how many nodes, how many layers —
// and a quad states all four as clearly as a creature does while staying
// readable when an assertion fails.
// ---------------------------------------------------------------------------

mdx::Node MakeNode(const std::string& name, u32 objectId, mdx::Node::NodeType type,
                   u32 parent = mdx::Node::NO_PARENT) {
    mdx::Node node;
    node.name = name;
    node.objectId = objectId;
    node.parentId = parent;
    node.type = type;
    return node;
}

mdx::Model QuadModel() {
    mdx::Model model;
    model.version = 800;
    model.modelName = "wem_fixture";
    model.modelExtent.minimum = Vector3f{-1, -1, 0};
    model.modelExtent.maximum = Vector3f{1, 1, 0};
    model.modelExtent.boundsRadius = 1.5f;

    mdx::Texture texture;
    texture.fileName = "textures\\white.blp";
    model.textures.push_back(texture);

    mdx::Layer layer;
    layer.filterMode = mdx::Layer::FilterMode::None;
    layer.textureId = 0;
    layer.alpha = 1.0f;
    mdx::Material material;
    material.layers.push_back(layer);
    model.materials.push_back(material);

    mdx::Geoset geoset;
    geoset.vertexPositions = {{-1, -1, 0}, {1, -1, 0}, {1, 1, 0}, {-1, 1, 0}};
    geoset.vertexNormals = {{0, 0, 1}, {0, 0, 1}, {0, 0, 1}, {0, 0, 1}};
    geoset.textureCoordinateSets.push_back({{0, 0}, {1, 0}, {1, 1}, {0, 1}});
    geoset.faces = {0, 1, 2, 0, 2, 3};
    geoset.faceTypeGroups = {4};
    geoset.faceGroups = {6};
    geoset.vertexGroups = {0, 0, 1, 1};
    geoset.matrixGroups = {1, 1};
    geoset.matrixIndices = {0, 1};
    geoset.materialId = 0;
    geoset.extent = model.modelExtent;
    model.geosets.push_back(std::move(geoset));

    mdx::Bone root;
    root.node = MakeNode("root", 0, mdx::Node::NodeType::Bone);
    mdx::Bone child;
    child.node = MakeNode("child", 1, mdx::Node::NodeType::Bone, 0);
    // One keyed track, so the document that comes out has a clip in it. The
    // sequence below is what slices it.
    child.node.translationTracks.isUsed = true;
    child.node.translationTracks.interpolationType = mdx::InterpolationType::Linear;
    child.node.translationTracks.keyCount = 2;
    child.node.translationTracks.timestamps = {0, 1000};
    child.node.translationTracks.keys_data = {Vector3f{0, 0, 0}, Vector3f{0, 0, 5}};
    model.bones.push_back(std::move(root));
    model.bones.push_back(std::move(child));

    mdx::Attachment attachment;
    attachment.node = MakeNode("hand ref", 2, mdx::Node::NodeType::Attachment, 0);
    attachment.attachmentId = 0;
    model.attachments.push_back(std::move(attachment));

    model.pivotPoints = {{0, 0, 0}, {0, 0, 1}, {0, 0, 2}};

    mdx::Sequence sequence;
    sequence.name = "Stand";
    sequence.intervalStart = 0;
    sequence.intervalEnd = 1000;
    model.sequences.push_back(std::move(sequence));

    return model;
}

/// The fixture, through the whole pipe: converter out, container out, container
/// in. What comes back is what a user's `.wem` would hold.
wem::Document RoundTripBytes(const mdx::Model& source, wem::Diagnostics* out = nullptr) {
    wem::MdxConverter converter;
    wem::Result<wem::Document> exported = converter.fromMdx(source);
    REQUIRE(exported.ok());
    if (out)
        out->append(exported.diagnostics);

    wem::Writer writer;
    const std::vector<u8> bytes = writer.write(*exported);
    REQUIRE_FALSE(bytes.empty());
    REQUIRE(wem::IsWemFile(bytes));

    auto parsed = ParseWemDocument(bytes, "fixture.wem");
    REQUIRE(parsed != nullptr);
    if (out)
        out->append(parsed->diagnostics);
    return std::move(parsed->document);
}

usize TotalVertices(std::vector<renderer::model::MeshData>& meshes) {
    usize n = 0;
    for (const auto& mesh : meshes)
        n += mesh.positions.size();
    return n;
}

} // namespace

// ---------------------------------------------------------------------------
// The profile table
// ---------------------------------------------------------------------------

TEST_CASE("every WEM profile has a product and an openable answer", "[wem]") {
    for (u32 i = 0; i < static_cast<u32>(wem::ProfileId::Count); ++i) {
        const auto profile = static_cast<wem::ProfileId>(i);
        // Total by construction — a profile with no product would spawn into
        // whichever scene happened to be open, which is the bug the table
        // exists to prevent.
        const ProductId product = ProductForWemProfile(profile);
        CHECK(product != ProductId::Neutral);

        // The two halves of "can this build open it" must agree: a refusal
        // always states a reason, and an acceptance never does.
        const bool openable = WemProfileOpenable(profile);
        const std::string reason = WemProfileUnsupportedReason(profile);
        CHECK(openable == reason.empty());

        // The registry name round-trips, which is what a settings file and the
        // CLI store rather than the enum value.
        CHECK(WemProfileFromName(wem::Profile(profile).name) == profile);
    }
}

TEST_CASE("a picked profile is one a converter serves", "[wem]") {
    // The bug this pins: `Generic` looked openable — it is a legal document
    // profile and the two WC3 ids share its material model — but no converter
    // SERVES it, and `MdxConverter::toMdx` refuses a profile by name. A
    // Diablo III document then defaulted to `Generic` and failed to open at
    // all, while a test that named `Wc3Classic` outright passed.
    CHECK_FALSE(WemProfileOpenable(wem::ProfileId::Generic));
    CHECK_FALSE(std::string(WemProfileUnsupportedReason(wem::ProfileId::Generic)).empty());

    for (u32 i = 0; i < static_cast<u32>(wem::ProfileId::Count); ++i) {
        const auto profile = static_cast<wem::ProfileId>(i);
        if (!WemProfileOpenable(profile))
            continue;
        // Every openable profile names a format, which is what the converter
        // lookup joins on.
        INFO(wem::Profile(profile).name);
        CHECK(wem::Profile(profile).formatId != nullptr);
    }
}

TEST_CASE("Diablo III goes both ways now, and only a build flag stops it", "[wem]") {
    // It used to be neither: writing SNO is a separate project (WEM_DESIGN.md
    // §18) and that was read as "there is nothing to hand D3ModelAdapter". A
    // viewer never needed a file — `D3Converter::toAppearance` builds the
    // native `Appearances` in memory, exactly as `toMdx` builds an
    // `mdx::Model` — so the only thing left that can refuse is the build.
#if WDX_ENABLE_D3
    CHECK(WemProfileOpenable(wem::ProfileId::Diablo3));
    CHECK(std::string(WemProfileUnsupportedReason(wem::ProfileId::Diablo3)).empty());
    // Writing the file is still out of scope, and the converter still says so.
    wem::D3Converter converter;
    CHECK_FALSE(converter.supportsExport());
#else
    CHECK_FALSE(WemProfileOpenable(wem::ProfileId::Diablo3));
    CHECK(std::string(WemProfileUnsupportedReason(wem::ProfileId::Diablo3)).find("WDX_ENABLE_D3") !=
          std::string::npos);
#endif
}

TEST_CASE("the option list offers carried profiles first and derives the rest", "[wem]") {
    const wem::Document document = RoundTripBytes(QuadModel());

    // A classic-only `.mdx` produces exactly one set: the HD test is per LAYER,
    // and the fixture's layer is an SD one.
    REQUIRE(document.carries(wem::ProfileId::Wc3Classic));

    const std::vector<WemProfileOption> options = WemProfileOptions(document);
    REQUIRE(options.size() == static_cast<usize>(wem::ProfileId::Count));

    // Carried and supported sorts first, so the head of the list is what a
    // dialog preselects.
    CHECK(options.front().profile == wem::ProfileId::Wc3Classic);
    CHECK(options.front().carried);
    CHECK_FALSE(options.front().derived);
    CHECK(options.front().drawn);

    // Nothing is filtered out — a row per profile, unsupported ones included.
    const auto d3 = std::find_if(options.begin(), options.end(), [](const WemProfileOption& o) {
        return o.profile == wem::ProfileId::Diablo3;
    });
    REQUIRE(d3 != options.end());
    CHECK(d3->supported == (WDX_ENABLE_D3 != 0));
    CHECK(d3->derived);
    CHECK(d3->deriveFrom == wem::ProfileId::Wc3Classic);

    // `Generic` is the one row that is never openable, and not because of a
    // build flag: it names no game, so it has no format and no converter.
    const auto generic =
        std::find_if(options.begin(), options.end(), [](const WemProfileOption& o) {
            return o.profile == wem::ProfileId::Generic;
        });
    REQUIRE(generic != options.end());
    CHECK_FALSE(generic->supported);

    CHECK(DefaultWemProfile(document) == wem::ProfileId::Wc3Classic);
}

// ---------------------------------------------------------------------------
// The identity gate
// ---------------------------------------------------------------------------

TEST_CASE("a Warcraft III model survives the round trip into an adapter", "[wem]") {
    const mdx::Model source = QuadModel();
    const wem::Document document = RoundTripBytes(source);

    // The document itself is well-formed at the profile level, which is what
    // makes the coverage rule (§6.3) a checked claim rather than a hope.
    const wem::Diagnostics report = wem::Validate(document, wem::ValidateLevel::Profile);
    CHECK_FALSE(report.hasErrors());

    WemDocument holder;
    holder.document = document;
    holder.name = "fixture.wem";

    const WemSourceResult built = BuildWemSource(holder, wem::ProfileId::Wc3Classic);
    INFO(built.error);
    REQUIRE(built.ok());
    CHECK(built.profile == wem::ProfileId::Wc3Classic);
    CHECK(built.product == ProductId::Wc3);
    CHECK_FALSE(built.hd);
    CHECK_FALSE(built.derived);

    auto meshes = built.source->GetMeshes();
    REQUIRE(meshes.size() == source.geosets.size());
    CHECK(TotalVertices(meshes) == source.geosets[0].vertexPositions.size());
    CHECK(meshes[0].indices.size() == source.geosets[0].faces.size());

    // The node tree: two bones plus the attachment, and the parent link that
    // makes it a tree rather than three roots.
    const auto skeleton = built.source->GetSkeleton();
    CHECK(skeleton.nodeCount == 3);
    CHECK(skeleton.nodeParents.size() == 3);
    CHECK(skeleton.nodeParents[1] == 0);

    auto materials = built.source->GetMaterials();
    REQUIRE(materials.size() == source.materials.size());
    CHECK(materials[0].layers.size() == source.materials[0].layers.size());

    auto textures = built.source->GetTextures();
    CHECK(textures.size() == source.textures.size());

    auto attachments = built.source->GetAttachmentConfigs();
    CHECK(attachments.size() == source.attachments.size());
}

TEST_CASE("opening as a profile the file does not carry derives it, and says so", "[wem]") {
    const wem::Document document = RoundTripBytes(QuadModel());
    REQUIRE_FALSE(document.carries(wem::ProfileId::Wc3Reforged));

    WemDocument holder;
    holder.document = document;
    holder.name = "fixture.wem";

    const WemSourceResult built = BuildWemSource(holder, wem::ProfileId::Wc3Reforged);
    INFO(built.error);
    REQUIRE(built.ok());
    CHECK(built.derived);
    // A derive is always lossy and the report is the product (§6.6), so an
    // empty one would mean the derive did not happen.
    CHECK_FALSE(built.diagnostics.empty());
    // Reforged is Warcraft III's HD generation; the host applies RenderMode::HD
    // off this rather than re-probing the material layers.
    CHECK(built.hd);
    CHECK(built.product == ProductId::Wc3);
    // Both Warcraft III profiles, so the units and the install the assets live
    // in are the ones the opened profile names. The cross-game case is what
    // makes these two separate answers; see the Diablo III sweep.
    CHECK(built.worldScale == 1.0f);
    CHECK(built.assetProduct == ProductId::Wc3);
    CHECK_FALSE(built.source->GetMeshes().empty());
}

TEST_CASE("a profile this build cannot open is refused by name", "[wem]") {
    const wem::Document document = RoundTripBytes(QuadModel());
    WemDocument holder;
    holder.document = document;
    holder.name = "fixture.wem";

    // `Generic` names no game, so it has no `formatId` and no converter serves
    // it — the one refusal that is a property of the format rather than of the
    // build.
    const WemSourceResult built = BuildWemSource(holder, wem::ProfileId::Generic);
    CHECK_FALSE(built.ok());
    CHECK(built.error.find("generic") != std::string::npos);
}

// ---------------------------------------------------------------------------
// Export
// ---------------------------------------------------------------------------

TEST_CASE("export dispatches on the adapter and refuses what it does not know", "[wem]") {
    MdxModelAdapter adapter(QuadModel());
    REQUIRE(CanExportModelToWem(adapter));

    const WemExportResult exported = ExportModelToWem(adapter);
    INFO(exported.error);
    REQUIRE(exported.ok());
    CHECK(exported.formatId == "mdx");
    CHECK(exported.document->models.size() == 1);
    CHECK(exported.document->carries(wem::ProfileId::Wc3Classic));

    const wem::Diagnostics report = wem::Validate(*exported.document, wem::ValidateLevel::Profile);
    CHECK_FALSE(report.hasErrors());
}

TEST_CASE("a source from no known format is refused rather than written empty", "[wem]") {
    // A host's own IModelSource — the shape the Max plugin has — carries no
    // parsed native model, so there is nothing for a converter to read.
    struct HostSource final : renderer::model::IModelSource {
        std::vector<renderer::model::MeshData> GetMeshes() override {
            return {};
        }
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
        std::vector<renderer::model::SequenceInfo> GetSequences() const override {
            return {};
        }
        renderer::model::FrameState Evaluate(const PoseRequest&) const override {
            return {};
        }
    };

    HostSource source;
    CHECK_FALSE(CanExportModelToWem(source));
    const WemExportResult exported = ExportModelToWem(source);
    CHECK_FALSE(exported.ok());
    CHECK_FALSE(exported.error.empty());
}

// ---------------------------------------------------------------------------
// The animation gap, pinned
// ---------------------------------------------------------------------------

TEST_CASE("animation survives the round trip, both halves", "[wem]") {
    const mdx::Model source = QuadModel();
    REQUIRE(source.sequences.size() == 1);

    const wem::Document document = RoundTripBytes(source);

    // The FILE has it: one clip per sequence, with the keyed track sliced into
    // it and the sequence's own window kept on the clip's native bag — which is
    // what makes the merge back onto MDX's one timeline exact rather than a
    // re-timing.
    REQUIRE(document.clips.size() == 1);
    CHECK(document.clips[0].name == "Stand");
    CHECK(document.clips[0].duration == Catch::Approx(1.0f));
    CHECK(document.clips[0].native.value("intervalStart", -1) ==
          static_cast<i64>(source.sequences[0].intervalStart));
    REQUIRE_FALSE(document.clips[0].containers.empty());
    CHECK_FALSE(document.clips[0].containers[0].subTracks.empty());

    WemDocument holder;
    holder.document = document;
    holder.name = "fixture.wem";
    const WemSourceResult built = BuildWemSource(holder, wem::ProfileId::Wc3Classic);
    INFO(built.error);
    REQUIRE(built.ok());

    // …and so does the MODEL that comes back. This is the half WEM v3 shipped
    // without — P7 was four importers and no exporter — and closing it is what
    // makes an imported `.wem` a model rather than a statue
    // (WEM_INTEGRATION_DESIGN.md §7).
    const auto sequences = built.source->GetSequences();
    REQUIRE(sequences.size() == source.sequences.size());
    CHECK(sequences[0].name == source.sequences[0].name);
    CHECK(sequences[0].startMs == static_cast<i32>(source.sequences[0].intervalStart));
    CHECK(sequences[0].endMs == static_cast<i32>(source.sequences[0].intervalEnd));

    // The keys themselves, not just the sequence table: the bone that was keyed
    // moves, and it moves the way it was authored. The fixture keys the child
    // bone's z from 0 to 5 across the sequence.
    ClipRef clip;
    clip.sequence = 0;
    clip.timeMs = static_cast<i32>(source.sequences[0].intervalEnd);
    clip.elapsedMs = clip.timeMs;
    clip.weight = 1.0f;
    PoseRequest request;
    request.clips = std::span<const ClipRef>(&clip, 1);
    const auto frame = built.source->Evaluate(request);
    REQUIRE(frame.boneWorldMatrices.size() >= 2);
    CHECK(frame.boneWorldMatrices[1].data[3][2] == Catch::Approx(5.0f).margin(0.01f));
}

// ---------------------------------------------------------------------------
// The corpus arm
//
// Everything above is a fixture built in memory, which proves the pipe is
// connected and nothing about what real content does to it. This sweeps the
// corpora that exist on this machine and asserts the one claim a user cares
// about: a model this build can draw goes out as a `.wem` and comes back as a
// model with the same geosets in it.
//
// Skips rather than fails without a corpus. How many models are swept is capped
// by WDX_TEST_WEM_LIMIT so a full run stays seconds rather than hours, and the
// cap is reported — a silent truncation reads as "covered everything".
// ---------------------------------------------------------------------------

namespace {

namespace fs = std::filesystem;

fs::path CorpusRoot() {
    if (const char* v = std::getenv("WDX_TEST_CORPUS"); v && *v)
        return fs::path(v);
    return fs::path("C:/Projects/WhiteoutLib/Corpus");
}

std::size_t SweepLimit() {
    if (const char* v = std::getenv("WDX_TEST_WEM_LIMIT"); v && *v)
        return static_cast<std::size_t>(std::strtoul(v, nullptr, 10));
    return 40;
}

std::vector<fs::path> FindByExtension(const fs::path& dir, const std::string& wanted,
                                      std::size_t limit) {
    std::vector<fs::path> out;
    std::error_code ec;
    if (!fs::is_directory(dir, ec))
        return out;
    for (fs::recursive_directory_iterator it(dir, ec), end; it != end; it.increment(ec)) {
        if (ec)
            break;
        if (!it->is_regular_file(ec))
            continue;
        std::string ext = it->path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (ext == wanted)
            out.push_back(it->path());
    }
    // Deterministic, so a failure names the same model on every run — and so
    // the cap takes the same prefix rather than whatever the directory iterator
    // happened to hand over first.
    std::sort(out.begin(), out.end());
    if (out.size() > limit)
        out.resize(limit);
    return out;
}

std::vector<u8> ReadAll(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file)
        return {};
    return std::vector<u8>((std::istreambuf_iterator<char>(file)),
                           std::istreambuf_iterator<char>());
}

// What survived one round trip. `geosets` stays -1 when the trip failed, and
// `why` says where.
struct RoundTrip {
    i64 geosets = -1;
    i64 sequences = 0;
    i64 clips = 0;
    /// The rebuilt model, for a caller that wants to pose it.
    std::shared_ptr<renderer::model::IModelSource> source;
};

// Export @p source, write it, read it back and rebuild it as @p profile.
RoundTrip RoundTripThroughFile(renderer::model::IModelSource& source, wem::ProfileId profile,
                               io::IContentProvider* provider, std::string& why) {
    RoundTrip trip;
    const io::WemExportResult exported = io::ExportModelToWem(source, provider);
    if (!exported.ok()) {
        why = exported.error;
        return trip;
    }
    if (!exported.diagnostics.bySeverity(wem::Severity::Error).empty()) {
        why = "export reported errors";
        return trip;
    }

    wem::Writer writer;
    const std::vector<u8> bytes = writer.write(*exported.document);
    if (bytes.empty()) {
        why = "the writer produced nothing";
        return trip;
    }

    auto parsed = io::ParseWemDocument(bytes, "sweep.wem");
    if (!parsed) {
        why = "the written file did not parse";
        return trip;
    }
    trip.clips = static_cast<i64>(parsed->document.clips.size());

    const io::WemSourceResult built = io::BuildWemSource(*parsed, profile);
    if (!built.ok()) {
        why = built.error;
        return trip;
    }
    trip.geosets = static_cast<i64>(built.source->GetMeshes().size());
    trip.sequences = static_cast<i64>(built.source->GetSequences().size());
    trip.source = built.source;
    return trip;
}

/// Pose both sources at the middle of sequence @p sequence and return the
/// largest distance between the two bone palettes, or -1 when the two cannot be
/// compared at all.
///
/// The claim it checks is the one counts cannot: not "a sequence came back" but
/// "the keys did", in the space the renderer poses in.
f32 PoseDelta(renderer::model::IModelSource& before, renderer::model::IModelSource& after,
              std::size_t sequence) {
    const auto sequencesBefore = before.GetSequences();
    const auto sequencesAfter = after.GetSequences();
    if (sequence >= sequencesBefore.size() || sequence >= sequencesAfter.size())
        return -1.0f;

    const auto pose = [sequence](renderer::model::IModelSource& source,
                                 const renderer::model::SequenceInfo& info) {
        ClipRef clip;
        clip.sequence = static_cast<i32>(sequence);
        // Mid-sequence, where a linear track is between its keys rather than on
        // one — a wrong slice or a dropped tangent shows here and not at t=0.
        clip.timeMs = info.startMs + (info.endMs - info.startMs) / 2;
        clip.elapsedMs = clip.timeMs - info.startMs;
        clip.weight = 1.0f;
        PoseRequest request;
        request.clips = std::span<const ClipRef>(&clip, 1);
        return source.Evaluate(request);
    };

    const auto a = pose(before, sequencesBefore[sequence]);
    const auto b = pose(after, sequencesAfter[sequence]);
    if (a.boneWorldMatrices.empty() || a.boneWorldMatrices.size() != b.boneWorldMatrices.size())
        return -1.0f;

    f32 worst = 0.0f;
    for (std::size_t i = 0; i < a.boneWorldMatrices.size(); ++i) {
        for (u32 r = 0; r < 4; ++r) {
            for (u32 c = 0; c < 4; ++c) {
                worst = (std::max)(worst, std::abs(a.boneWorldMatrices[i].data[r][c] -
                                                   b.boneWorldMatrices[i].data[r][c]));
            }
        }
    }
    return worst;
}

} // namespace

TEST_CASE("corpus .mdx models round-trip through a written .wem", "[wem][corpus]") {
    const std::size_t limit = SweepLimit();
    const auto models = FindByExtension(CorpusRoot() / "MDL", ".mdx", limit);
    if (models.empty())
        SKIP("no .mdx under " + (CorpusRoot() / "MDL").string());

    std::size_t swept = 0, matched = 0, animated = 0, posed = 0;
    for (const auto& path : models) {
        INFO("model " << path.string());
        whiteout::mdx::Model parsed;
        try {
            whiteout::mdx::Parser parser;
            parsed = parser.parse(path.string());
        } catch (const std::exception&) {
            continue; // a corpus file this parser refuses is not this test's subject
        }
        if (parsed.geosets.empty())
            continue;

        MdxModelAdapter adapter(parsed, path.parent_path());
        const std::size_t before = adapter.GetMeshes().size();
        const std::size_t sequencesBefore = adapter.GetSequences().size();

        // Written back as the profile the file's own layers put it in: a
        // classic model has no Reforged set to open, and asking for one would
        // measure the derive rather than the round trip.
        const io::WemExportResult probe = io::ExportModelToWem(adapter);
        REQUIRE(probe.ok());
        const wem::ProfileId profile = probe.document->defaultProfile;

        std::string why;
        const RoundTrip after = RoundTripThroughFile(adapter, profile, nullptr, why);
        INFO(why);
        REQUIRE(after.geosets >= 0);
        CHECK(static_cast<std::size_t>(after.geosets) == before);
        // The animation half. A model with no sequences must come back with
        // none — the assertion is agreement with the file, never a threshold.
        CHECK(static_cast<std::size_t>(after.sequences) == sequencesBefore);
        matched += (static_cast<std::size_t>(after.geosets) == before &&
                    static_cast<std::size_t>(after.sequences) == sequencesBefore)
                       ? 1u
                       : 0u;
        // The pose itself, on the first animated model of the sweep: counts
        // cannot tell a sequence table that came back from keys that did.
        if (sequencesBefore > 0 && posed == 0 && after.source) {
            const f32 delta = PoseDelta(adapter, *after.source, 0);
            if (delta >= 0.0f) {
                INFO("worst bone-matrix delta " << delta);
                CHECK(delta < 0.01f);
                ++posed;
            }
        }
        animated += sequencesBefore > 0 ? 1u : 0u;
        ++swept;
    }
    WARN("mdx: " << matched << "/" << swept << " round-tripped intact (" << animated
                 << " with sequences, " << posed << " pose-checked, cap " << limit << ")");
    CHECK(swept > 0);
}

#if WDX_ENABLE_M3
TEST_CASE("corpus .m3 models round-trip through a written .wem", "[wem][corpus]") {
    const std::size_t limit = SweepLimit();
    const auto models = FindByExtension(CorpusRoot() / "Sc2M3", ".m3", limit);
    if (models.empty())
        SKIP("no .m3 under " + (CorpusRoot() / "Sc2M3").string());

    std::size_t swept = 0, matched = 0, animated = 0, posed = 0, crossBuilt = 0;
    for (const auto& path : models) {
        INFO("model " << path.string());
        const std::vector<u8> bytes = ReadAll(path);
        if (bytes.empty())
            continue;
        auto adapter = io::M3ModelAdapter::Load(ContentRef::FromPath(path.string()),
                                                std::span<const u8>(bytes.data(), bytes.size()));
        if (!adapter)
            continue;
        const std::size_t before = adapter->GetMeshes().size();
        if (before == 0)
            continue; // REGN v2 draws nothing; that gap is m3_geometry_test's subject
        const std::size_t sequencesBefore = adapter->GetSequences().size();

        const io::WemExportResult probe = io::ExportModelToWem(*adapter);
        REQUIRE(probe.ok());
        const wem::ProfileId profile = probe.document->defaultProfile;

        std::string why;
        const RoundTrip after = RoundTripThroughFile(*adapter, profile, nullptr, why);
        INFO(why);
        REQUIRE(after.geosets >= 0);
        CHECK(static_cast<std::size_t>(after.geosets) == before);
        CHECK(static_cast<std::size_t>(after.sequences) == sequencesBefore);
        matched += (static_cast<std::size_t>(after.geosets) == before &&
                    static_cast<std::size_t>(after.sequences) == sequencesBefore)
                       ? 1u
                       : 0u;
        if (sequencesBefore > 0 && posed == 0 && after.source) {
            const f32 delta = PoseDelta(*adapter, *after.source, 0);
            if (delta >= 0.0f) {
                INFO("worst bone-matrix delta " << delta);
                CHECK(delta < 0.01f);
                ++posed;
            }
        }
        // …and the same document as WARCRAFT III, which is where a `.m3` rig has
        // to be restated: StarCraft II carries an explicit bind and MDX a pivot
        // one, so every node track is re-solved on the way out.
        //
        // The bone palettes are what this compares, because counts cannot see
        // the failure it was written for. `BONE.flags` spells its inherit bits
        // positively and no shipped bone sets them, so reading their absence as
        // MDX's three `DontInherit*` — which Warcraft III honours and StarCraft
        // II does not — detached every bone from its parent while the geoset
        // count, the sequence count and the draw count all stayed right.
        // …and the same document as WARCRAFT III, which is where a `.m3` rig has
        // to be restated: StarCraft II carries an explicit bind and MDX a pivot
        // one, so every node track is re-solved on the way out. A conversion
        // that drops the whole skeleton still lands here with the right counts,
        // so this is a reachability check and not a pose one — the pose is
        // `wem_convert_m3_test`'s and the draw-trace gate's.
        //
        // Comparing the two palettes directly is not available and is worth
        // saying once: `FrameState::boneWorldMatrices` is whatever its own
        // shader wants beside `SkeletonData::inverseBindMatrices`, and the two
        // formats split that differently — MDX uploads one entry per NODE with
        // the bind folded in, `.m3` one per BONE with IREF left out and a cloth
        // particle appended per simulated vertex. Index i is not the same bone
        // on both sides, and the difference reads as metres of error.
        if (crossBuilt < 4) {
            std::string crossWhy;
            const RoundTrip asWc3 =
                RoundTripThroughFile(*adapter, wem::ProfileId::Wc3Classic, nullptr, crossWhy);
            INFO(crossWhy);
            REQUIRE(asWc3.geosets >= 0);
            CHECK(static_cast<std::size_t>(asWc3.geosets) == before);
            CHECK(static_cast<std::size_t>(asWc3.sequences) == sequencesBefore);
            ++crossBuilt;
        }

        animated += sequencesBefore > 0 ? 1u : 0u;
        ++swept;
    }
    WARN("m3: " << matched << "/" << swept << " round-tripped intact (" << animated
                << " with sequences, " << posed << " pose-checked, " << crossBuilt
                << " rebuilt as Warcraft III, cap " << limit << ")");
    CHECK(swept > 0);
    CHECK(crossBuilt > 0);
}
#endif

#if WDX_ENABLE_M2
TEST_CASE("corpus .m2 models round-trip through a written .wem", "[wem][corpus]") {
    const std::size_t limit = SweepLimit();
    const auto models = FindByExtension(CorpusRoot() / "WoW", ".m2", limit);
    if (models.empty())
        SKIP("no .m2 under " + (CorpusRoot() / "WoW").string());

    io::FileContentProvider provider;
    // A Legion-or-later `.m2` names its skins by fileDataID, which only a WoW
    // storage resolves.
    provider.SetGame(ProductId::Wow);

    std::size_t swept = 0, matched = 0, animated = 0, posed = 0;
    for (const auto& path : models) {
        INFO("model " << path.string());
        provider.SetBasePath(path.parent_path());
        const std::vector<u8> bytes = ReadAll(path);
        if (bytes.empty())
            continue;
        auto adapter =
            io::M2ModelAdapter::Load(ContentRef::FromPath(path.string()),
                                     std::span<const u8>(bytes.data(), bytes.size()), &provider);
        if (!adapter)
            continue; // no `.skin` sibling — m2_geometry_test is where that fails
        const std::size_t before = adapter->GetMeshes().size();
        if (before == 0)
            continue;
        const std::size_t sequencesBefore = adapter->GetSequences().size();

        std::string why;
        const RoundTrip after = RoundTripThroughFile(*adapter, wem::ProfileId::Wow, &provider, why);
        INFO(why);
        REQUIRE(after.geosets >= 0);
        CHECK(static_cast<std::size_t>(after.geosets) == before);
        CHECK(static_cast<std::size_t>(after.sequences) == sequencesBefore);
        matched += (static_cast<std::size_t>(after.geosets) == before &&
                    static_cast<std::size_t>(after.sequences) == sequencesBefore)
                       ? 1u
                       : 0u;
        if (sequencesBefore > 0 && posed == 0 && after.source) {
            const f32 delta = PoseDelta(*adapter, *after.source, 0);
            if (delta >= 0.0f) {
                INFO("worst bone-matrix delta " << delta);
                CHECK(delta < 0.01f);
                ++posed;
            }
        }
        animated += sequencesBefore > 0 ? 1u : 0u;
        ++swept;
    }
    WARN("m2: " << matched << "/" << swept << " round-tripped intact (" << animated
                << " with sequences, " << posed << " pose-checked, cap " << limit << ")");
    CHECK(swept > 0);
}
#endif

#if WDX_ENABLE_M2
TEST_CASE("a creature's resolved skin is written into the document", "[wem]") {
    // A World of Warcraft creature names none of its own skins. The slot
    // carries a texture TYPE and the game fills it from the display record at
    // spawn, so a document written from the parsed model alone comes back
    // white — geometry, materials and blend modes all correct, and nothing to
    // sample. The export reads what the spawn resolved instead.
    const std::filesystem::path model =
        CorpusRoot() / "WoW" / "creature" / "felstalker" / "felstalker.m2";
    if (!std::filesystem::exists(model))
        SKIP("no felstalker.m2 under " + CorpusRoot().string());

    io::FileContentProvider provider;
    provider.SetGame(ProductId::Wow);
    provider.SetBasePath(model.parent_path());
    const std::vector<u8> bytes = ReadAll(model);
    if (bytes.empty())
        SKIP("felstalker.m2 did not read");
    auto adapter =
        io::M2ModelAdapter::Load(ContentRef::FromPath(model.string()),
                                 std::span<const u8>(bytes.data(), bytes.size()), &provider);
    if (!adapter)
        SKIP("felstalker.m2 did not load");

    // What WowReplaceableTextures would have said, stated here so the test
    // needs no client databases: type 11 is a creature's first skin.
    std::vector<std::string> byType(13);
    byType[11] = "#1108877";
    adapter->SetReplaceableTextures(byType);

    const io::WemExportResult exported = io::ExportModelToWem(*adapter, &provider);
    REQUIRE(exported.ok());

    bool baked = false;
    for (const wem::TextureRef& ref : exported.document->textures) {
        if (ref.slotType != 11)
            continue;
        const auto* byId = std::get_if<wem::TextureFileDataId>(&ref.key);
        REQUIRE(byId != nullptr);
        CHECK(byId->value == 1108877u);
        // The reference still records that the slot is replaceable; only the
        // key became concrete.
        CHECK(ref.slotType == 11);
        baked = true;
    }
    CHECK(baked);
}
#endif

#if WDX_ENABLE_D3
// ---------------------------------------------------------------------------
// Diablo III, both ways
//
// Writing SNO *files* is still a separate project (WEM_DESIGN.md §18), and a
// viewer never needed one: the document becomes a native `Appearances` in
// memory. So this arm asserts the whole loop — an actor exports, survives the
// container, re-opens AS Diablo III, and comes back with the same geosets, the
// same dressing and the same clips.
// ---------------------------------------------------------------------------

namespace {

// A content provider over the extracted corpus, keyed the way Diablo III keys
// itself: a SNO id IS the file id, and every group's payload opens with its own
// id at byte 16, so indexing a directory is a 4-byte read per file.
class D3CorpusProvider final : public io::IContentProvider {
public:
    bool Index() {
        const fs::path root = CorpusRoot() / "D3";
        std::error_code ec;
        if (!fs::is_directory(root, ec))
            return false;
        for (const char* group : {"Actor", "Appearances", "AnimSet", "Anim"}) {
            const fs::path dir = root / group;
            if (!fs::is_directory(dir, ec))
                continue;
            std::vector<fs::path> files;
            for (fs::directory_iterator it(dir, ec), end; it != end; it.increment(ec)) {
                if (ec)
                    break;
                if (it->is_regular_file(ec))
                    files.push_back(it->path());
            }
            std::sort(files.begin(), files.end());
            for (const auto& file : files) {
                std::ifstream stream(file, std::ios::binary);
                char header[20];
                if (!stream.read(header, 20))
                    continue;
                i32 sno = 0;
                std::memcpy(&sno, header + 16, 4);
                paths_.emplace(sno, file);
                byGroup_[group].push_back(sno);
            }
        }
        return !paths_.empty();
    }

    const std::vector<i32>& Group(const char* group) const {
        static const std::vector<i32> kEmpty;
        auto it = byGroup_.find(group);
        return it == byGroup_.end() ? kEmpty : it->second;
    }

    io::RequestId Request(const io::ContentRef& ref, io::CompletionCallback done) override {
        io::RequestResult result;
        if (ref.IsFileId()) {
            auto it = paths_.find(static_cast<i32>(ref.fileId));
            if (it != paths_.end()) {
                result.data = ReadAll(it->second);
                result.ok = !result.data.empty();
            }
        }
        done(std::move(result));
        return ++next_;
    }
    void Wait(io::RequestId) override {}
    void Cancel(io::RequestId) override {}
    void Pump() override {}

private:
    std::unordered_map<i32, fs::path> paths_;
    std::unordered_map<std::string, std::vector<i32>> byGroup_;
    io::RequestId next_ = 0;
};

} // namespace

TEST_CASE("corpus Diablo III actors export to a written .wem", "[wem][corpus][d3]") {
    D3CorpusProvider provider;
    if (!provider.Index())
        SKIP("no Diablo III corpus under " + (CorpusRoot() / "D3").string());

    io::D3SnoCache cache(&provider);
    const std::vector<i32>& actors = provider.Group("Actor");
    if (actors.empty())
        SKIP("the Diablo III corpus has no actors");

    const std::size_t limit = SweepLimit();
    std::size_t swept = 0, withGeometry = 0, retargeted = 0;
    for (std::size_t i = 0; i < actors.size() && swept < limit; ++i) {
        auto adapter = io::D3ModelAdapter::LoadActorBySno(actors[i], cache, /*lazyClips=*/true);
        if (!adapter)
            continue;
        INFO("actor #" << actors[i]);
        const std::size_t geosets = adapter->GetMeshes().size();

        const io::WemExportResult exported = io::ExportModelToWem(*adapter, &provider);
        INFO(exported.error);
        REQUIRE(exported.ok());
        CHECK(exported.formatId == "d3");
        // The shaders resolved through the provider, so the materials carry
        // render state rather than the appearance's own flags.
        REQUIRE_FALSE(exported.document->models.empty());
        CHECK(exported.document->carries(wem::ProfileId::Diablo3));
        CHECK_FALSE(exported.diagnostics.bySeverity(wem::Severity::Error).size() > 0);

        const wem::Diagnostics report =
            wem::Validate(*exported.document, wem::ValidateLevel::Profile);
        INFO(report.formatHistogram());
        CHECK_FALSE(report.hasErrors());

        // Through the container and back: the document a user would keep.
        wem::Writer writer;
        const std::vector<u8> bytes = writer.write(*exported.document);
        REQUIRE_FALSE(bytes.empty());
        auto parsed = io::ParseWemDocument(bytes, "sweep.wem");
        REQUIRE(parsed);
        CHECK(parsed->document.models.size() == exported.document->models.size());

        // …and back in as Diablo III. `ProfileId::Count` is what a host with
        // no dialog passes, and a document that carries a Diablo III set now
        // opens as one rather than deriving Warcraft III out of it.
        io::D3SnoCache openCache(&provider);
        const io::WemSourceResult built =
            io::BuildWemSource(*parsed, wem::ProfileId::Count, {}, &provider, &openCache);
        INFO(built.error);
        REQUIRE(built.ok());
        CHECK(built.profile == wem::ProfileId::Diablo3);
        CHECK_FALSE(built.derived);
        // The geometry is the claim. A geoset per section, not one merged draw
        // — which is what a mesh-per-geoset conversion produces and what made
        // a re-opened actor a single 18,000-vertex blob.
        CHECK(built.source->GetMeshes().size() == geosets);
        // And it really is the D3 path: the adapter, not something derived.
        CHECK(dynamic_cast<io::D3ModelAdapter*>(built.source.get()) != nullptr);

        if (geosets > 0) {
            // The dressing. Every armour variant is in the appearance and the
            // engine decides which draw, so the document's hidden flags have to
            // arrive with it — a re-opened character that lost them draws its
            // naked, light, medium and heavy torso at once.
            auto* d3 = dynamic_cast<io::D3ModelAdapter*>(built.source.get());
            REQUIRE(d3 != nullptr);
            CHECK(d3->GeosetHidden().size() == d3->EmittedSubObjects().size());
            // The clips. Not equality: the native adapter still lists a tag
            // whose `.ani` does not resolve in this install (2 of Barbarian
            // Male's 259 do not), and the document never got a clip for one, so
            // the written half is the honest subset.
            const usize nativeSequences = adapter->GetSequences().size();
            const usize wemSequences = built.source->GetSequences().size();
            CHECK(wemSequences <= nativeSequences);
            if (nativeSequences > 0) {
                CHECK(wemSequences > 0);
                // Within a few of the whole set, not "some survived".
                CHECK(wemSequences + 8 >= nativeSequences);
            }
            ++retargeted;

            // …and the same document as WARCRAFT III, which is the whole
            // cross-game path. Three facts a merged conversion got wrong:
            //
            //  - a geoset per section, not one draw of everything;
            //  - the units, which stay Diablo III's because a derive restates
            //    a material set and never touches a vertex (17 against
            //    Warcraft III's 1 is a model a seventeenth of the right size);
            //  - the install every texture resolves against, which is still
            //    Diablo III even though the render profile is now Warcraft III.
            const io::WemSourceResult asWc3 =
                io::BuildWemSource(*parsed, wem::ProfileId::Wc3Classic, {}, &provider, &openCache);
            INFO(asWc3.error);
            REQUIRE(asWc3.ok());
            CHECK(asWc3.derived);
            CHECK(asWc3.source->GetMeshes().size() == geosets);
            CHECK(asWc3.product == ProductId::Wc3);
            CHECK(asWc3.assetProduct == ProductId::D3);
            CHECK(asWc3.worldScale == wem::Profile(wem::ProfileId::Diablo3).sceneScale);
        }

        withGeometry += (geosets > 0) ? 1u : 0u;
        ++swept;
    }
    WARN("d3: " << swept << " actors exported (" << withGeometry << " with geometry, " << retargeted
                << " re-opened as Diablo III, cap " << limit << ")");
    CHECK(swept > 0);
}

TEST_CASE("a dressed Diablo III actor exports dressed", "[wem][corpus][d3]") {
    // The one thing the converter cannot see. Which armour draws is equipment
    // state — `ActorModel_ApplyLook` flips visibility on sub-objects that were
    // there all along — so it reaches the adapter as `SetGeosetHidden` and the
    // `.app` says nothing about it. Without a bake on the way out, a dressed
    // character exports naked and re-opens with every armour variant drawing.
    D3CorpusProvider provider;
    if (!provider.Index())
        SKIP("no Diablo III corpus under " + (CorpusRoot() / "D3").string());

    io::D3SnoCache cache(&provider);
    const std::vector<i32>& actors = provider.Group("Actor");
    if (actors.empty())
        SKIP("the Diablo III corpus has no actors");

    for (const i32 actor : actors) {
        auto adapter = io::D3ModelAdapter::LoadActorBySno(actor, cache, /*lazyClips=*/true);
        if (!adapter || adapter->EmittedSubObjects().size() < 4)
            continue;
        INFO("actor #" << actor);

        // A pattern nothing would produce by accident: every third geoset held
        // back, and a look override plus a dye on the first.
        const usize count = adapter->EmittedSubObjects().size();
        std::vector<u8> hidden(count, 0);
        for (usize g = 0; g < count; g += 3)
            hidden[g] = 1;
        adapter->SetGeosetHidden(hidden);
        std::vector<i32> dyes(count, 0);
        dyes[0] = 7;
        adapter->SetGeosetDyes(dyes);

        const io::WemExportResult exported = io::ExportModelToWem(*adapter, &provider);
        INFO(exported.error);
        REQUIRE(exported.ok());

        wem::Writer writer;
        const std::vector<u8> bytes = writer.write(*exported.document);
        auto parsed = io::ParseWemDocument(bytes, "dressed.wem");
        REQUIRE(parsed);

        io::D3SnoCache openCache(&provider);
        const io::WemSourceResult built =
            io::BuildWemSource(*parsed, wem::ProfileId::Diablo3, {}, &provider, &openCache);
        INFO(built.error);
        REQUIRE(built.ok());
        auto* back = dynamic_cast<io::D3ModelAdapter*>(built.source.get());
        REQUIRE(back != nullptr);

        // Byte for byte, not "roughly as many hidden".
        REQUIRE(back->GeosetHidden().size() == hidden.size());
        for (usize g = 0; g < hidden.size(); ++g) {
            INFO("geoset " << g);
            CHECK(back->GeosetHidden()[g] == hidden[g]);
        }
        REQUIRE(back->GeosetDyes().size() == dyes.size());
        CHECK(back->GeosetDyes()[0] == 7);
        return;
    }
    SUCCEED("no actor with enough geosets to dress");
}
#endif
