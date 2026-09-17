// ============================================================================
// The conversion library's shared plumbing, and each exporter end to end.
//
// Device-free and corpus-free: the textures are made in memory, the model is
// a hand-built quad, and every file lands in a temporary folder.
// ============================================================================

#include <catch2/catch_test_macros.hpp>

#include "export/baked_texture.h"
#include "export/export_common.h"
#include "export/gltf_export.h"
#include "export/m3_export.h"
#include "export/mdx_export.h"
#include "export/texture_codec.h"
#include "export/texture_io.h"

#include "io/mdx_model_adapter.h"
#include "io/wem/wem_export.h"
#include "whiteout/flakes/util/path_utf8.h"

#include <whiteout/textures/texture.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <set>
#include <span>
#include <string>

using namespace whiteout;
using namespace whiteout::flakes;
namespace fs = std::filesystem;
namespace tx = ::whiteout::textures;
namespace wem = ::whiteout::models::wem;

namespace {

/// A folder of its own under the system temp directory, removed afterwards.
struct TempDir {
    TempDir() {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path = fs::temp_directory_path() / ("viewer_convert_test_" + std::to_string(stamp));
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    fs::path path;
};

/// One quad over one bone, one classic layer.
mdx::Model QuadModel() {
    mdx::Model model;
    model.version = 800;
    model.modelName = "convert_fixture";
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
    geoset.vertexGroups = {0, 0, 0, 0};
    geoset.matrixGroups = {1};
    geoset.matrixIndices = {0};
    geoset.materialId = 0;
    geoset.extent = model.modelExtent;
    model.geosets.push_back(std::move(geoset));

    mdx::Bone root;
    root.node.name = "root";
    root.node.objectId = 0;
    root.node.parentId = mdx::Node::NO_PARENT;
    root.node.type = mdx::Node::NodeType::Bone;
    root.geosetId = 0;
    model.bones.push_back(root);
    model.pivotPoints.push_back(Vector3f{0, 0, 0});
    return model;
}

/// A checker of fully clear and fully solid 2x2 cells in alpha, over a red
/// ramp — the alpha a keyed bake must keep binary.
tx::Texture KeyedChecker(u32 size) {
    tx::Texture texture = tx::Texture::create2D(tx::PixelFormat::RGBA8, size, size, 1);
    const std::span<u8> px = texture.mipData(0);
    for (u32 y = 0; y < size; ++y) {
        for (u32 x = 0; x < size; ++x) {
            const std::size_t at = (static_cast<std::size_t>(y) * size + x) * 4;
            px[at] = static_cast<u8>(x * 255 / (size - 1));
            px[at + 1] = 128;
            px[at + 2] = 64;
            px[at + 3] = ((x / 2 + y / 2) % 2 == 0) ? 255 : 0;
        }
    }
    return texture;
}

/// The distinct alpha values of level @p mip of a written `.dds`.
std::set<u8> AlphaValues(const std::vector<u8>& dds, u32 mip) {
    TextureDecode decoded = DecodeTexture(dds, ".DDS");
    REQUIRE(decoded.texture.has_value());
    const tx::Texture rgba = decoded.texture->copyAsFormat(tx::PixelFormat::RGBA8);
    REQUIRE(rgba.mipCount() > mip);
    std::set<u8> values;
    const std::span<const u8> px = rgba.mipData(mip);
    for (std::size_t i = 3; i < px.size(); i += 4)
        values.insert(px[i]);
    return values;
}

} // namespace

TEST_CASE("an id-addressed texture's key round-trips to its file id", "[convert]") {
    wem::TextureRef byFileId;
    byFileId.key = wem::TextureFileDataId{158949};
    CHECK(TextureIdKey(byFileId) == "#158949");
    const ContentRef ref = ContentRefForKey(TextureIdKey(byFileId));
    CHECK(ref.IsFileId());
    CHECK(ref.fileId == 158949u);

    wem::TextureRef bySno;
    bySno.key = wem::TextureSnoId{/*group=*/44, /*id=*/4242};
    CHECK(TextureIdKey(bySno) == "#4242");
    CHECK(ContentRefOf(bySno).fileId == 4242u);

    wem::TextureRef byPath;
    byPath.path = "Textures\\Footman.blp";
    byPath.key = wem::TexturePath{byPath.path};
    CHECK(TextureIdKey(byPath).empty());
    CHECK(ContentRefForKey(byPath.path).IsPath());
    CHECK(ContentRefForKey("#").IsPath()); // a lone '#' names no id
}

TEST_CASE("texture counters sum every field", "[convert]") {
    TextureExportCounters total{1, 2, 3, 4, 5};
    total += TextureExportCounters{10, 20, 30, 40, 50};
    CHECK(total.exported == 11);
    CHECK(total.skipped == 22);
    CHECK(total.failed == 33);
    CHECK(total.unused == 44);
    CHECK(total.inWar3Mod == 55);
}

TEST_CASE("a decode reports why it failed instead of throwing", "[convert]") {
    const std::vector<u8> junk = {1, 2, 3, 4};
    const TextureDecode unknown = DecodeTexture(junk, ".xyz");
    CHECK_FALSE(unknown.texture.has_value());
    CHECK_FALSE(unknown.problem.empty());
    CHECK_FALSE(static_cast<bool>(EncodeTextureAs(KeyedChecker(8), ".nope")));
}

TEST_CASE("a keyed bake keeps a binary alpha down its whole chain", "[convert][mips]") {
    const BakedTexture keyed(KeyedChecker(64), tx::PixelFormat::BC3,
                             {tx::TextureKind::Diffuse, tx::TextureKind::BinaryMask, true});
    const EncodedTexture encoded = EncodeBaked(keyed);
    INFO(encoded.problem);
    REQUIRE(static_cast<bool>(encoded));
    CHECK(encoded.note.empty());
    for (u32 mip = 0; mip < 4; ++mip) {
        INFO("mip " << mip);
        for (const u8 alpha : AlphaValues(encoded.bytes, mip))
            CHECK((alpha == 0 || alpha == 255));
    }

    // The same pixels box-filtered, as every bake was before it declared a
    // kind: the smaller levels go soft.
    const EncodedTexture boxed =
        EncodeBaked(BakedTexture::BoxFiltered(KeyedChecker(64), tx::PixelFormat::BC3));
    REQUIRE(static_cast<bool>(boxed));
    // Level 2 averages four checker cells; level 1 stays inside one.
    const std::set<u8> soft = AlphaValues(boxed.bytes, 2);
    CHECK(std::any_of(soft.begin(), soft.end(), [](u8 a) { return a != 0 && a != 255; }));
}

TEST_CASE("an authored chain is written as it was made", "[convert][mips]") {
    tx::Texture probe = tx::Texture::create2D(tx::PixelFormat::RGBA8, 16, 16, 3);
    for (u32 mip = 0; mip < probe.mipCount(); ++mip) {
        const std::span<u8> px = probe.mipData(mip);
        std::fill(px.begin(), px.end(), static_cast<u8>(mip == 1 ? 0 : 255));
    }
    const EncodedTexture encoded =
        EncodeBaked(BakedTexture::WithAuthoredChain(std::move(probe), tx::PixelFormat::BC3));
    REQUIRE(static_cast<bool>(encoded));
    // Level 1 is the black the bake authored, not the white a rebuild from
    // level 0 would give.
    CHECK(AlphaValues(encoded.bytes, 1) == std::set<u8>{0});
    CHECK(AlphaValues(encoded.bytes, 0) == std::set<u8>{255});
}

TEST_CASE("a bake that declares no kind is named", "[convert][mips]") {
    const EncodedTexture encoded = EncodeBaked(BakedTexture::Declared(
        tx::Texture::create2D(tx::PixelFormat::RGBA8, 8, 8, 1), tx::PixelFormat::BC3));
    REQUIRE(static_cast<bool>(encoded));
    CHECK_FALSE(encoded.note.empty());
}

TEST_CASE("the WEM hop appends to the report it is given", "[convert]") {
    io::MdxModelAdapter adapter(QuadModel());
    ConversionReport report;
    report.diagnostics.info(wem::DiagCode::Unspecified, "said before the hop");
    std::optional<wem::Document> document =
        ConvertThroughWem(ExportSubject{&adapter, nullptr, {}, "fixture"}, io::WemExportOptions{}, report);
    REQUIRE(document.has_value());
    CHECK(report.formatId == "mdx");
    REQUIRE_FALSE(report.diagnostics.empty());
    CHECK(report.diagnostics.all().front().message == "said before the hop");
}

TEST_CASE("a model is written by each exporter", "[convert][export]") {
    TempDir temp;
    io::MdxModelAdapter adapter(QuadModel());

    SECTION("mdx") {
        MdxExportRequest request;
        request.subject = {&adapter, nullptr, temp.path / "quad.mdx", "quad"};
        const MdxExportReport report = ExportModelAsMdx(request);
        INFO(report.error);
        REQUIRE(report.ok);
        CHECK(fs::file_size(temp.path / "quad.mdx") > 0);
        // No provider: the one texture is named and counted, not written.
        CHECK(report.textures.failed == 1);
    }
    SECTION("m3") {
        M3ExportRequest request;
        request.subject = {&adapter, nullptr, temp.path / "quad.m3", "quad"};
        request.options.textures = false;
        const M3ExportReport report = ExportModelAsM3(request);
        INFO(report.error);
        REQUIRE(report.ok);
        CHECK(fs::file_size(temp.path / "quad.m3") > 0);
        CHECK(report.scale != 1.0f); // Warcraft III to StarCraft II units
    }
    SECTION("glb") {
        GltfExportRequest request;
        request.subject = {&adapter, nullptr, temp.path / "quad.glb", "quad"};
        const GltfExportReport report = ExportModelAsGltf(request);
        INFO(report.error);
        REQUIRE(report.ok);
        CHECK(fs::file_size(temp.path / "quad.glb") > 0);
    }
    SECTION("a folder that does not exist yet") {
        GltfExportRequest request;
        request.subject = {&adapter, nullptr, temp.path / "a" / "b" / "quad.gltf", "quad"};
        request.options.binary = false;
        const GltfExportReport report = ExportModelAsGltf(request);
        INFO(report.error);
        REQUIRE(report.ok);
        CHECK(fs::exists(temp.path / "a" / "b" / "quad.gltf"));
    }
}
