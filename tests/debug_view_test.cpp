// Debug views, the device-free half (DEBUG_VIEW_DESIGN.md): which passes a
// view keeps, which menu lists it, the integer the hosts persist, and the
// constant-buffer layout every debug shader reads.

#include <catch2/catch_test_macros.hpp>

#include "core/debug_view.h"
#include "renderer/render_settings.h"
#include "whiteout/flakes/enums.h"

#include <cstddef>

using namespace whiteout::flakes;
using renderer::RenderSettings;
using renderer::core::DebugFrame;
using renderer::core::DebugViewCbData;
using renderer::core::DebugViewKind;
using renderer::core::KindOf;
using renderer::core::ResolveDebugFrame;

namespace {

constexpr u8 kLastView = static_cast<u8>(DebugView::WireframeTeamColor);

} // namespace

TEST_CASE("Off leaves every pass alone", "[debug_view]") {
    const DebugFrame f = ResolveDebugFrame(DebugView::Off);
    CHECK_FALSE(f.Active());
    CHECK_FALSE(f.debugSurfaces);
    CHECK(f.gtao);
    CHECK_FALSE(f.gtaoAoOnly);
    CHECK(f.deferredLights);
    CHECK(f.bloom);
    CHECK(f.dof);
    CHECK(f.refraction);
    CHECK(f.distortion);
    CHECK(f.tonemap);
    CHECK(f.effects);
}

TEST_CASE("A channel view keeps nothing that repaints colour", "[debug_view]") {
    for (DebugView v :
         {DebugView::Albedo, DebugView::Normal, DebugView::TextureMip, DebugView::LightCount,
          DebugView::Roughness, DebugView::Emissive, DebugView::VertexNormal, DebugView::Opacity}) {
        CAPTURE(static_cast<int>(v));
        const DebugFrame f = ResolveDebugFrame(v);
        CHECK(KindOf(v) == DebugViewKind::Channel);
        CHECK(f.debugSurfaces);
        CHECK_FALSE(f.gtao);
        CHECK_FALSE(f.deferredLights);
        CHECK_FALSE(f.bloom);
        CHECK_FALSE(f.dof);
        CHECK_FALSE(f.refraction);
        CHECK_FALSE(f.distortion);
        // The copy, so the channel reaches the screen as computed.
        CHECK_FALSE(f.tonemap);
        CHECK_FALSE(f.effects);
    }
}

TEST_CASE("A lighting view keeps the lights and the tonemap", "[debug_view]") {
    for (DebugView v : {DebugView::LightingWhite, DebugView::LightingGrey, DebugView::SpecularOnly,
                        DebugView::NoOrm}) {
        CAPTURE(static_cast<int>(v));
        const DebugFrame f = ResolveDebugFrame(v);
        CHECK(KindOf(v) == DebugViewKind::Lighting);
        CHECK(f.debugSurfaces);
        CHECK(f.deferredLights);
        CHECK(f.tonemap);
        // The debug programs write no G-buffer for GTAO to read.
        CHECK_FALSE(f.gtao);
        CHECK_FALSE(f.bloom);
        CHECK_FALSE(f.effects);
    }
}

TEST_CASE("A wireframe view keeps nothing that repaints colour", "[debug_view]") {
    for (DebugView v :
         {DebugView::Wireframe, DebugView::WireframeVertices, DebugView::WireframeTeamColor}) {
        CAPTURE(static_cast<int>(v));
        const DebugFrame f = ResolveDebugFrame(v);
        CHECK(KindOf(v) == DebugViewKind::Wireframe);
        CHECK(f.Active());
        CHECK(f.overlay.Any());
        CHECK(f.overlay.edges);
        CHECK_FALSE(f.gtao);
        CHECK_FALSE(f.deferredLights);
        CHECK_FALSE(f.bloom);
        CHECK_FALSE(f.tonemap);
        CHECK_FALSE(f.effects);
    }
}

TEST_CASE("Only the unshaded wireframe draws the surfaces, as Albedo", "[debug_view]") {
    const DebugFrame bare = ResolveDebugFrame(DebugView::Wireframe);
    CHECK_FALSE(bare.drawSurfaces);
    CHECK_FALSE(bare.debugSurfaces);
    CHECK_FALSE(bare.overlay.occluded);
    CHECK_FALSE(bare.overlay.vertices);
    CHECK_FALSE(bare.overlay.faces);

    const DebugFrame unshaded = ResolveDebugFrame(DebugView::WireframeVertices);
    CHECK(unshaded.drawSurfaces);
    CHECK(unshaded.debugSurfaces);
    CHECK(unshaded.surfaceView == DebugView::Albedo);
    CHECK(unshaded.overlay.vertices);
    CHECK(unshaded.overlay.occluded);
    CHECK_FALSE(unshaded.overlay.faces);
    // The surfaces' debug programs are told Albedo, not the wireframe value
    // they have no answer for.
    const auto cb = renderer::core::MakeDebugViewCb(unshaded, {}, 0, 0);
    CHECK(cb.view == static_cast<u32>(DebugView::Albedo));

    const DebugFrame team = ResolveDebugFrame(DebugView::WireframeTeamColor);
    CHECK_FALSE(team.drawSurfaces);
    CHECK(team.overlay.faces);
    CHECK(team.overlay.occluded);
    CHECK_FALSE(team.overlay.vertices);

    // Every other view draws its surfaces and no overlay.
    for (u8 v = 0; v < static_cast<u8>(DebugView::Wireframe); ++v) {
        CAPTURE(int(v));
        const DebugFrame f = ResolveDebugFrame(static_cast<DebugView>(v));
        CHECK(f.drawSurfaces);
        CHECK_FALSE(f.overlay.Any());
        CHECK(f.surfaceView == f.view);
    }
}

TEST_CASE("AO Only draws the real shaders under the GTAO factor", "[debug_view]") {
    const DebugFrame f = ResolveDebugFrame(DebugView::AoOnly);
    CHECK_FALSE(f.debugSurfaces);
    CHECK(f.gtao);
    CHECK(f.gtaoAoOnly);
    CHECK_FALSE(f.tonemap);
}

TEST_CASE("Each family lists exactly its views", "[debug_view]") {
    const auto pbr = DebugViewFamily::Pbr;
    const auto legacy = DebugViewFamily::Legacy;
    for (u8 v = 0; v <= kLastView; ++v) {
        CAPTURE(int(v));
        const auto view = static_cast<DebugView>(v);
        // Nothing the enum names is unreachable from both menus.
        CHECK((DebugViewInFamily(view, pbr) || DebugViewInFamily(view, legacy)));
    }
    CHECK(DebugViewInFamily(DebugView::Off, pbr));
    CHECK(DebugViewInFamily(DebugView::Off, legacy));
    CHECK(DebugViewInFamily(DebugView::Albedo, legacy));
    CHECK(DebugViewInFamily(DebugView::Emissive, legacy));
    CHECK_FALSE(DebugViewInFamily(DebugView::Roughness, legacy));
    CHECK_FALSE(DebugViewInFamily(DebugView::AoOnly, legacy));
    CHECK_FALSE(DebugViewInFamily(DebugView::TeamMask, legacy));
    CHECK_FALSE(DebugViewInFamily(DebugView::Specular, pbr));
    CHECK_FALSE(DebugViewInFamily(DebugView::Opacity, pbr));
    // Geometry has no family.
    for (DebugView v :
         {DebugView::Wireframe, DebugView::WireframeVertices, DebugView::WireframeTeamColor}) {
        CHECK(DebugViewInFamily(v, pbr));
        CHECK(DebugViewInFamily(v, legacy));
    }
}

TEST_CASE("The old integer keeps its meaning and rejects the rest", "[debug_view]") {
    RenderSettings s;
    // 0-9 are the values HdDebugMode always had; ini files and the npm package
    // still send them.
    s.SetHdDebugMode(9);
    CHECK(s.GetDebugView() == DebugView::AoOnly);
    for (i32 v = 0; v <= kLastView; ++v) {
        s.SetHdDebugMode(v);
        CHECK(s.HdDebugMode() == v);
    }
    s.SetDebugView(DebugView::Gloss);
    s.SetHdDebugMode(kLastView + 1);
    CHECK(s.GetDebugView() == DebugView::Off);
    s.SetDebugView(DebugView::Gloss);
    s.SetHdDebugMode(-1);
    CHECK(s.GetDebugView() == DebugView::Off);
}

TEST_CASE("DebugViewCbData is DebugViewData's layout", "[debug_view]") {
    // shaders/debug_view.slang: uint4 ctl; float4 params.
    CHECK(sizeof(DebugViewCbData) == 32);
    CHECK(offsetof(DebugViewCbData, view) == 0);
    CHECK(offsetof(DebugViewCbData, targetFlags) == 4);
    CHECK(offsetof(DebugViewCbData, lightCount) == 8);
    CHECK(offsetof(DebugViewCbData, productFlags) == 12);
    CHECK(offsetof(DebugViewCbData, lightCountRedAt) == 16);

    DebugFrame f = ResolveDebugFrame(DebugView::Specular);
    renderer::core::DebugTargetInfo t;
    t.colorSamplesLinear = true;
    t.targetEncodesSrgb = true;
    const DebugViewCbData d = renderer::core::MakeDebugViewCb(f, t, 3, 7, 5.0f);
    CHECK(d.view == static_cast<u32>(DebugView::Specular));
    CHECK(d.targetFlags == 3u);
    CHECK(d.lightCount == 3u);
    CHECK(d.productFlags == 7u);
    CHECK(d.lightCountRedAt == 5.0f);
}
