// Mesh overlay, the device-free half (DEBUG_VIEW_DESIGN.md §9): the edges a
// triangle list yields, how element states combine, the element buffer the
// overlay vertex stages pull from, and the constant-buffer layout they read.

#include <catch2/catch_test_macros.hpp>

#include "core/mesh_overlay.h"

#include <cmath>
#include <cstddef>
#include <vector>

using namespace whiteout::flakes;
using namespace whiteout::flakes::renderer::core;

namespace {

// Two triangles sharing the 1-2 diagonal of a unit quad.
const std::vector<u32> kQuad = {0, 1, 2, 2, 1, 3};

const MeshEdge* Find(const std::vector<MeshEdge>& edges, u32 a, u32 b) {
    for (const auto& e : edges)
        if (e.a == a && e.b == b)
            return &e;
    return nullptr;
}

} // namespace

TEST_CASE("Edges are unique, ordered, and know their faces", "[mesh_overlay]") {
    const auto edges = BuildMeshEdges(kQuad, 4);
    REQUIRE(edges.size() == 5);
    for (const auto& e : edges)
        CHECK(e.a < e.b);
    const MeshEdge* diagonal = Find(edges, 1, 2);
    REQUIRE(diagonal);
    CHECK(diagonal->faces[0] == 0);
    CHECK(diagonal->faces[1] == 1);
    const MeshEdge* border = Find(edges, 0, 1);
    REQUIRE(border);
    CHECK(border->faces[0] == 0);
    CHECK(border->faces[1] == kMeshNoFace);
}

TEST_CASE("Degenerate and out-of-range triangles give no bad edges", "[mesh_overlay]") {
    // A sliver with a repeated corner has one real edge; a triangle naming a
    // vertex the geoset lacks contributes nothing.
    const std::vector<u32> indices = {0, 0, 1, 0, 1, 9};
    const auto edges = BuildMeshEdges(indices, 4);
    REQUIRE(edges.size() == 1);
    CHECK(edges[0].a == 0);
    CHECK(edges[0].b == 1);
    CHECK(edges[0].faces[1] == kMeshNoFace);
}

TEST_CASE("Flags apply under their mask and grow on first use", "[mesh_overlay]") {
    MeshElementStates s;
    const u32 ids[] = {1, 3, 7};
    // Clearing an untouched geoset allocates nothing.
    CHECK_FALSE(SetMeshElementFlags(s, MeshElementKind::Vertex, 4, ids, kMeshElementSelected, 0));
    CHECK(s.vertices.empty());

    CHECK(SetMeshElementFlags(s, MeshElementKind::Vertex, 4, ids, kMeshElementSelected,
                              kMeshElementSelected));
    REQUIRE(s.vertices.size() == 4);
    CHECK(s.vertices[1] == kMeshElementSelected);
    CHECK(s.vertices[3] == kMeshElementSelected);
    CHECK(s.vertices[0] == 0);
    // Id 7 is past the geoset and ignored.

    const u32 one[] = {1};
    CHECK(SetMeshElementFlags(s, MeshElementKind::Vertex, 4, one, kMeshElementHovered,
                              kMeshElementHovered));
    CHECK(s.vertices[1] == (kMeshElementSelected | kMeshElementHovered));
    CHECK_FALSE(SetMeshElementFlags(s, MeshElementKind::Vertex, 4, one, kMeshElementHovered,
                                    kMeshElementHovered));
    CHECK(SetMeshElementFlags(s, MeshElementKind::Vertex, 4, one, kMeshElementSelected, 0));
    CHECK(s.vertices[1] == kMeshElementHovered);
    CHECK(s.faces.empty());
}

TEST_CASE("An edge is marked by both ends or by a face beside it", "[mesh_overlay]") {
    const auto edges = BuildMeshEdges(kQuad, 4);
    const MeshEdge& diagonal = *Find(edges, 1, 2);
    const MeshEdge& border = *Find(edges, 0, 1);

    MeshElementStates s;
    const u32 v1[] = {1};
    SetMeshElementFlags(s, MeshElementKind::Vertex, 4, v1, kMeshElementSelected,
                        kMeshElementSelected);
    CHECK(MeshEdgeState(diagonal, s) == 0);
    const u32 v2[] = {2};
    SetMeshElementFlags(s, MeshElementKind::Vertex, 4, v2, kMeshElementSelected,
                        kMeshElementSelected);
    CHECK(MeshEdgeState(diagonal, s) == kMeshElementSelected);
    CHECK(MeshEdgeState(border, s) == 0);

    MeshElementStates f;
    const u32 face1[] = {1};
    SetMeshElementFlags(f, MeshElementKind::Face, 2, face1, kMeshElementHovered,
                        kMeshElementHovered);
    CHECK(MeshEdgeState(diagonal, f) == kMeshElementHovered);
    CHECK(MeshEdgeState(border, f) == 0);
    CHECK(AnyMarkedFace(f));
    CHECK_FALSE(AnyMarkedFace(s));
}

TEST_CASE("An edge hides with an end or with every face beside it", "[mesh_overlay]") {
    const auto edges = BuildMeshEdges(kQuad, 4);
    const MeshEdge& diagonal = *Find(edges, 1, 2);
    const MeshEdge& border = *Find(edges, 0, 1);

    MeshElementStates s;
    const u32 face0[] = {0};
    SetMeshElementFlags(s, MeshElementKind::Face, 2, face0, kMeshElementHidden, kMeshElementHidden);
    CHECK(MeshEdgeState(border, s) == kMeshElementHidden);
    // Still beside a visible face.
    CHECK(MeshEdgeState(diagonal, s) == 0);

    const u32 face1[] = {1};
    SetMeshElementFlags(s, MeshElementKind::Face, 2, face1, kMeshElementHidden, kMeshElementHidden);
    CHECK(MeshEdgeState(diagonal, s) == kMeshElementHidden);

    MeshElementStates v;
    const u32 v0[] = {0};
    SetMeshElementFlags(v, MeshElementKind::Vertex, 4, v0, kMeshElementHidden, kMeshElementHidden);
    CHECK(MeshEdgeState(border, v) == kMeshElementHidden);
    CHECK(MeshEdgeState(diagonal, v) == 0);
}

TEST_CASE("The element buffer is what the vertex stages pull", "[mesh_overlay]") {
    const std::vector<Vector3f> positions = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {1, 1, 0.5f}};
    // Vertex 3 skins through bones 7 and 2 at 128/255 and 127/255.
    std::vector<u8> weights(16, 0);
    std::vector<u8> bones(16, 0);
    weights[12] = 128;
    weights[13] = 127;
    bones[12] = 7;
    bones[13] = 2;
    const auto edges = BuildMeshEdges(kQuad, 4);
    const auto layout = MeshOverlayLayoutFor(4, static_cast<u32>(edges.size()), 2);
    CHECK(layout.vertexBase == 0);
    CHECK(layout.edgeBase == 12);
    CHECK(layout.faceBase == 17);
    CHECK(layout.elementCount == 19);

    MeshElementStates s;
    const u32 v3[] = {3};
    SetMeshElementFlags(s, MeshElementKind::Vertex, 4, v3, kMeshElementSelected,
                        kMeshElementSelected);
    const u32 f1[] = {1};
    SetMeshElementFlags(s, MeshElementKind::Face, 2, f1, kMeshElementHovered, kMeshElementHovered);

    std::vector<f32> out(layout.elementCount * 4, -1.0f);
    PackMeshOverlay(out,
                    {.positions = positions,
                     .boneWeights = weights,
                     .boneIndices = bones,
                     .indices = kQuad,
                     .edges = edges},
                    &s, layout);

    const f32* v = &out[3 * 3 * 4];
    CHECK(v[0] == 1.0f);
    CHECK(v[1] == 1.0f);
    CHECK(v[2] == 0.5f);
    CHECK(v[3] == static_cast<f32>(kMeshElementSelected));
    // The input stage's UNORM divide, bit for bit.
    CHECK(v[4] == 128.0f / 255.0f);
    CHECK(v[5] == 127.0f / 255.0f);
    CHECK(v[8] == 7.0f);
    CHECK(v[9] == 2.0f);

    const f32* face = &out[(layout.faceBase + 1) * 4];
    CHECK(face[0] == 2.0f);
    CHECK(face[1] == 1.0f);
    CHECK(face[2] == 3.0f);
    CHECK(face[3] == static_cast<f32>(kMeshElementHovered));

    const f32* edge = &out[layout.edgeBase * 4];
    CHECK(edge[0] == static_cast<f32>(edges[0].a));
    CHECK(edge[1] == static_cast<f32>(edges[0].b));
    CHECK(edge[2] == static_cast<f32>(MeshEdgeState(edges[0], s)));
}

TEST_CASE("A rigid geoset packs zero weights, a broken triangle packs hidden", "[mesh_overlay]") {
    const std::vector<Vector3f> positions = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}};
    const std::vector<u32> indices = {0, 1, 2, 0, 1, 5};
    const auto edges = BuildMeshEdges(indices, 3);
    const auto layout = MeshOverlayLayoutFor(3, static_cast<u32>(edges.size()), 2);
    std::vector<f32> out(layout.elementCount * 4, -1.0f);
    PackMeshOverlay(out, {.positions = positions, .indices = indices, .edges = edges}, nullptr,
                    layout);
    for (u32 i = 0; i < 3; ++i) {
        CHECK(out[(i * 3 + 1) * 4] == 0.0f);
        CHECK(out[(i * 3 + 2) * 4] == 0.0f);
        CHECK(out[i * 3 * 4 + 3] == 0.0f);
    }
    CHECK(out[(layout.faceBase + 1) * 4 + 3] == static_cast<f32>(kMeshElementHidden));
}

TEST_CASE("MeshOverlayCbData is MeshOverlayData's layout", "[mesh_overlay]") {
    // shaders/mesh_overlay.slang: uint4 ctl, uint4 sections, float4 viewport,
    // float4 depth, then five float4 colours.
    CHECK(sizeof(MeshOverlayCbData) == 144);
    CHECK(offsetof(MeshOverlayCbData, pass) == 0);
    CHECK(offsetof(MeshOverlayCbData, flags) == 4);
    CHECK(offsetof(MeshOverlayCbData, vertexBase) == 16);
    CHECK(offsetof(MeshOverlayCbData, edgeBase) == 20);
    CHECK(offsetof(MeshOverlayCbData, faceBase) == 24);
    CHECK(offsetof(MeshOverlayCbData, halfViewport) == 32);
    CHECK(offsetof(MeshOverlayCbData, edgeWidth) == 40);
    CHECK(offsetof(MeshOverlayCbData, vertexSize) == 44);
    CHECK(offsetof(MeshOverlayCbData, depthPull) == 48);
    CHECK(offsetof(MeshOverlayCbData, faceColor) == 64);
    CHECK(offsetof(MeshOverlayCbData, edgeColor) == 80);
    CHECK(offsetof(MeshOverlayCbData, vertexColor) == 96);
    CHECK(offsetof(MeshOverlayCbData, selectedColor) == 112);
    CHECK(offsetof(MeshOverlayCbData, hoveredColor) == 128);
}

TEST_CASE("The depth pull is a fraction of the eye distance either way round", "[mesh_overlay]") {
    const f32 n = 10.0f;
    const f32 f = 5000.0f;
    const auto rh = Matrix44f::perspective_fov_rh(0.8f, 1.5f, n, f);
    const auto lh = Matrix44f::perspective_fov_lh(0.8f, 1.5f, n, f);
    const f32 expected = 0.01f * f * n / (f - n);
    CHECK(MeshOverlayDepthPull(rh, 0.01f) == MeshOverlayDepthPull(lh, 0.01f));
    CHECK(std::abs(MeshOverlayDepthPull(rh, 0.01f) - expected) < 1e-4f);

    // Moving a point 1% toward the eye moves its NDC depth by pull / w, to
    // first order (the exact step is 1/0.99 - 1 of it).
    auto ndc = [&](f32 zView) {
        const f32 zc = rh.data[2][2] * zView + rh.data[3][2];
        return zc / -zView;
    };
    const f32 z = -800.0f;
    const f32 moved = ndc(z * 0.99f) - ndc(z);
    CHECK(std::abs(moved + MeshOverlayDepthPull(rh, 0.01f) / 800.0f) < 2e-5f);
}

TEST_CASE("A bare wireframe reads against the background", "[mesh_overlay]") {
    const u32 red = 0x0000FFu;
    const auto dark = MeshOverlayColorsFor(DebugView::Wireframe, red, 0x202020u);
    const auto light = MeshOverlayColorsFor(DebugView::Wireframe, red, 0xE0E0E0u);
    CHECK(dark.edge[0] > 0.5f);
    CHECK(light.edge[0] < 0.5f);
    // Over faces the edges are dark whatever the background.
    const auto team = MeshOverlayColorsFor(DebugView::WireframeTeamColor, red, 0x202020u);
    CHECK(team.edge[0] < 0.1f);
    CHECK(team.face[0] == 1.0f);
    CHECK(team.face[1] == 0.0f);
    CHECK(team.face[2] == 0.0f);
}
