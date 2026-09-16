#pragma once

// Debug views, frame half: which passes a view keeps and what every debug
// pixel shader is told. The surface half is shaders/debug_view.slang, fed by
// one debug entry per product (DEBUG_VIEW_DESIGN.md).
//
// Device-free on purpose — the whole table is a G0 test.

#include "core/mesh_overlay.h"
#include "whiteout/flakes/enums.h"
#include "whiteout/flakes/types.h"

namespace whiteout::flakes::renderer::core {

using ::whiteout::flakes::DebugView;

enum class DebugViewKind : u8 {
    Off = 0,
    // A material channel. Nothing after the surfaces may touch the colour.
    Channel = 1,
    // Radiance with an overridden material: the frame's own lighting and
    // tonemap stay, only what paints over them goes.
    Lighting = 2,
    // The real shaders, with GTAO writing its factor over the result.
    AmbientOcclusion = 3,
    // Geometry: the mesh overlay's edges, vertices and flat faces, over the
    // unshaded surfaces or instead of them. Nothing after may touch the colour.
    Wireframe = 4,
};

inline constexpr DebugViewKind KindOf(DebugView v) {
    switch (v) {
    case DebugView::Off:
        return DebugViewKind::Off;
    case DebugView::LightingWhite:
    case DebugView::LightingGrey:
    case DebugView::SpecularOnly:
    case DebugView::NoOrm:
        return DebugViewKind::Lighting;
    case DebugView::AoOnly:
        return DebugViewKind::AmbientOcclusion;
    case DebugView::Wireframe:
    case DebugView::WireframeVertices:
    case DebugView::WireframeTeamColor:
        return DebugViewKind::Wireframe;
    default:
        return DebugViewKind::Channel;
    }
}

struct DebugFrame {
    // The view the host picked.
    DebugView view = DebugView::Off;
    // The channel a debug pixel shader shows: `view`, except that a wireframe
    // view over unshaded surfaces shows Albedo.
    DebugView surfaceView = DebugView::Off;
    // Surfaces bind their debug pixel shader rather than the real one.
    bool debugSurfaces = false;
    // False: a model still binds each surface's state and emits its overlay,
    // but draws no triangles of its own.
    bool drawSurfaces = true;
    MeshOverlayStyle overlay;
    // Run the GTAO pass at all; its own enable still decides the rest.
    bool gtao = true;
    bool gtaoAoOnly = false;
    bool deferredLights = true;
    bool bloom = true;
    bool dof = true;
    bool refraction = true;
    bool distortion = true;
    // False replaces the tonemap with a straight copy.
    bool tonemap = true;
    // Particles, ribbons, corn and splats. They have no material channels and
    // would paint over the surfaces being inspected.
    bool effects = true;

    bool Active() const {
        return view != DebugView::Off;
    }
};

inline constexpr DebugFrame ResolveDebugFrame(DebugView v) {
    DebugFrame f;
    f.view = v;
    f.surfaceView = v;
    const DebugViewKind kind = KindOf(v);
    if (kind == DebugViewKind::Off)
        return f;

    f.bloom = false;
    f.dof = false;
    f.refraction = false;
    f.distortion = false;
    f.effects = false;
    switch (kind) {
    case DebugViewKind::Channel:
        f.debugSurfaces = true;
        f.gtao = false;
        f.deferredLights = false;
        f.tonemap = false;
        break;
    case DebugViewKind::Lighting:
        // The debug programs write no G-buffer, so GTAO would read a cleared
        // one; the M3 deferred lights read the albedo the MRT twin overrides.
        f.debugSurfaces = true;
        f.gtao = false;
        break;
    case DebugViewKind::AmbientOcclusion:
        f.gtaoAoOnly = true;
        f.deferredLights = false;
        f.tonemap = false;
        break;
    case DebugViewKind::Wireframe:
        f.overlay = MeshOverlayStyleFor(v);
        // The unshaded surfaces are the Albedo channel view, drawn as that
        // view draws them; the other two leave the surfaces to the overlay.
        f.drawSurfaces = v == DebugView::WireframeVertices;
        f.debugSurfaces = f.drawSurfaces;
        f.surfaceView = f.drawSurfaces ? DebugView::Albedo : v;
        f.gtao = false;
        f.deferredLights = false;
        f.tonemap = false;
        break;
    case DebugViewKind::Off:
        break;
    }
    return f;
}

// What a debug pixel shader needs from the frame, beyond the view itself.
struct DebugTargetInfo {
    // Colour textures were acquired through sRGB views (the scene is float),
    // so a sample is linear and has to be re-encoded to show the texel.
    bool colorSamplesLinear = false;
    // The render target view gamma-encodes on write.
    bool targetEncodesSrgb = false;
};

// DebugViewData in shaders/debug_view.slang.
struct DebugViewCbData {
    u32 view = 0;
    u32 targetFlags = 0; // 1 colour samples linear, 2 target encodes sRGB
    u32 lightCount = 0;
    u32 productFlags = 0;
    f32 lightCountRedAt = 8.0f;
    f32 params1 = 0.0f;
    f32 params2 = 0.0f;
    f32 params3 = 0.0f;
};

inline DebugViewCbData MakeDebugViewCb(const DebugFrame& frame, const DebugTargetInfo& target,
                                       u32 lightCount, u32 productFlags,
                                       f32 lightCountRedAt = 8.0f) {
    DebugViewCbData d;
    d.view = static_cast<u32>(frame.surfaceView);
    d.targetFlags = (target.colorSamplesLinear ? 1u : 0u) | (target.targetEncodesSrgb ? 2u : 0u);
    d.lightCount = lightCount;
    d.productFlags = productFlags;
    d.lightCountRedAt = lightCountRedAt;
    return d;
}

} // namespace whiteout::flakes::renderer::core
