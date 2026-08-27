#pragma once

// ClassifyGeoset — pure classification of a geoset by WC3's rules (IsOpaque in
// ModelRender.cpp): find the first *visible* layer, decide opaque vs transparent
// from its blend mode, and report skinned / multilayer. No GPU, no side effects;
// the collection stage (BuildDrawLists) turns this into opaque/transparent draw
// items. Kept separate so the rule is one place and testable on its own.

#include "whiteout/flakes/types.h"

namespace whiteout::flakes::renderer::model {
struct GPUGeoset;
}

namespace whiteout::flakes::renderer::render_detail {
struct RenderableView;
}

// Shared by BOTH WC3 shading models. The shaderId-based HD fading test stays
// inside: Wc3SdShading::Classify must still apply it to a shaderId-1 layer,
// because that is what the rule does today regardless of render mode.
// Splitting it per model is a behaviour change and belongs to the
// mixed-shading work, not here.
namespace whiteout::flakes::renderer::profiles::wc3 {

struct GeosetClass {
    bool visible = false;       // some layer is currently visible
    i32 firstVisibleLayer = -1; // index of that layer (-1 if none)
    bool opaque = true;         // first visible layer blends < Blend (Opaque/AlphaKey)
    bool needsDepthFill = false; // HD opaque layer faded below full → Color path + depth twin
    // The blend-mode half of `opaque`, before the HD fading demotion. A fading
    // HD layer is still a solid surface that occludes — `opaque` says which
    // scene queue it draws in, this says what the geometry *is*. The shadow
    // pass reads it: a fading unit keeps casting, an additive glow never does.
    bool opaqueFilter = true;
};

GeosetClass ClassifyGeoset(const render_detail::RenderableView& view,
                           const model::GPUGeoset& geo);

} // namespace whiteout::flakes::renderer::profiles::wc3
