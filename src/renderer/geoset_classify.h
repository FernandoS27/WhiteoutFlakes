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

struct GeosetClass {
    bool visible = false;       // some layer is currently visible
    i32 firstVisibleLayer = -1; // index of that layer (-1 if none)
    bool opaque = true;         // first visible layer blends < Blend (Opaque/AlphaKey)
};

GeosetClass ClassifyGeoset(const RenderableView& view, const model::GPUGeoset& geo);

} // namespace whiteout::flakes::renderer::render_detail
