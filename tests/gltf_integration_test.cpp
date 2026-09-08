// ============================================================================
// glTF import/export, from the renderer's side (GLTF_DESIGN §11, gate shape).
//
// The claim under test is §2's whole architecture: a model goes out through
// its own converter, through WEM, out as a `.glb`, back in as a Generic
// document, and opens through the EXISTING `.wem` machinery — profile options,
// the Reforged derive, `BuildWemSource` — into the same MDX adapter the
// renderer draws. Zero new renderer code is the design; this is the test that
// the seams it reused actually carry it.
//
// Device-free and corpus-free: the fixture is built in memory, so this runs on
// a CI box with no game installed and no GPU.
// ============================================================================

#include "io/wem/wem_export.h"
#include "io/wem/wem_import.h"
#include "io/wem/wem_profiles.h"

#include "io/mdx_model_adapter.h"

#include <whiteout/models/wem/converters.h>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using namespace whiteout;
using namespace whiteout::flakes;
using namespace whiteout::flakes::io;
namespace wem = ::whiteout::models::wem;

namespace {

/// One quad over one bone, classic layers — the smallest model whose crossing
/// still has geometry, a skin, a material and a node to lose.
mdx::Model QuadModel() {
    mdx::Model model;
    model.version = 800;
    model.modelName = "gltf_fixture";
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

} // namespace

TEST_CASE("a model crosses to glb and back into the adapter that draws it", "[gltf]") {
    // Out: the adapter the renderer holds, through WEM, into a `.glb`.
    MdxModelAdapter adapter(QuadModel());
    REQUIRE(CanExportModelToWem(adapter));
    const WemExportResult exported = ExportModelToWem(adapter);
    INFO(exported.error);
    REQUIRE(exported.ok());

    const wem::GltfConverter converter;
    wem::Result<std::vector<u8>> glb =
        converter.exportToBytes(*exported.document, wem::ProfileId::Wc3Classic);
    REQUIRE(glb.ok());
    CHECK(LooksLikeGlb(*glb));

    // Back: the parse lands on Generic — the neutral profile §6.1 reserved for
    // exactly this — and every open option is a derive.
    std::shared_ptr<WemDocument> reimported = ParseGltfDocument(*glb, "fixture.glb");
    REQUIRE(reimported != nullptr);
    REQUIRE(reimported->document.profiles.size() == 1);
    CHECK(reimported->document.profiles[0] == wem::ProfileId::Generic);
    CHECK(DefaultWemProfile(reimported->document) == wem::ProfileId::Wc3Reforged);
    for (const WemProfileOption& option : WemProfileOptions(reimported->document)) {
        CHECK(option.derived == (option.profile != wem::ProfileId::Generic));
    }

    // And into the adapter, through the same call the viewer's open makes:
    // the Generic set derives into Reforged, which draws HD under Warcraft
    // III's product with no new renderer code.
    const WemSourceResult built = BuildWemSource(*reimported, wem::ProfileId::Count);
    INFO(built.error);
    REQUIRE(built.ok());
    CHECK(built.profile == wem::ProfileId::Wc3Reforged);
    CHECK(built.product == ProductId::Wc3);
    CHECK(built.hd);
    CHECK(built.derived);
    CHECK_FALSE(built.diagnostics.empty());

    const auto meshes = built.source->GetMeshes();
    REQUIRE(meshes.size() == 1);
    CHECK(meshes[0].indices.size() == 6);
}

TEST_CASE("the gltf predicates answer by magic and by name", "[gltf]") {
    CHECK(LooksLikeGltfPath("model.gltf"));
    CHECK(LooksLikeGltfPath("MODEL.GLB"));
    CHECK_FALSE(LooksLikeGltfPath("model.mdx"));
    CHECK_FALSE(LooksLikeGltfPath("model.wem"));

    const u8 notGlb[] = {'{', '}', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    CHECK_FALSE(LooksLikeGlb(notGlb));
}
