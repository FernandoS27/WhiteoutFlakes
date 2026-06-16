#pragma once

// Draw-list value types + sort comparators for the geoset passes. Pure data and
// pure ordering — no GPU, no behaviour — so the comparators are unit-testable
// and the collection (BuildDrawLists), classification (ClassifyGeoset) and
// submission (DrawLayer) stages communicate only through these POD items.
//
// Ordering mirrors WC3 (ModelRender.cpp): opaque is a state-batching sort whose
// correctness comes from the depth buffer; transparent is back-to-front by
// camera distance with the Depth prepass twin drawn just before its Color draw.

#include "bls/layer_material.h" // bls::DepthFill
#include "whiteout/flakes/types.h"

#include <vector>

namespace whiteout::flakes::renderer::render_detail {

struct RenderableView;

// One opaque draw: a whole geoset (all its visible layers, drawn in layer order
// so a base layer precedes any additive detail on top). Correctness comes from
// the depth buffer — drawing whole geosets matches per-layer splitting visually,
// and fits both the SD and the HD (internal-prepass) submission paths.
struct OpaqueItem {
    const RenderableView* view = nullptr;
    i32 geoIdx = -1;
};

// One transparent draw (always the whole geoset), sorted back-to-front. depthFill
// stays None today — SD never depth-fills and HD does its fade internally; the
// field is the seam for a future WC3-exact HD Color/Depth split.
struct TransparentItem {
    const RenderableView* view = nullptr;
    i32 geoIdx = -1;
    bls::DepthFill depthFill = bls::DepthFill::None;
    f32 sqDist = 0.0f; // squared camera distance (back-to-front key)
    i32 priorityPlane = 0;
    bool underWater = false;
};

struct DrawLists {
    std::vector<OpaqueItem> opaque;
    std::vector<TransparentItem> transparent;
};

// Opaque order: correctness is the depth buffer's job, so this is only a stable,
// per-model grouping. (WC3 additionally batches by texture/material to cut state
// changes; that's a perf optimization we can layer on later.)
inline bool OpaqueOrder(const OpaqueItem& a, const OpaqueItem& b) {
    if (a.view != b.view)
        return a.view < b.view;
    return a.geoIdx < b.geoIdx;
}

// Transparent order mirrors CTransparentObject::HasHigherPriority: underwater
// first, priorityPlane ascending, distance back-to-front, then the Depth
// prepass twin (2) before its Color draw (1) at the same position.
inline bool TransparentOrder(const TransparentItem& a, const TransparentItem& b) {
    if (a.underWater != b.underWater)
        return a.underWater;
    if (a.priorityPlane != b.priorityPlane)
        return a.priorityPlane < b.priorityPlane;
    if (a.sqDist != b.sqDist)
        return a.sqDist > b.sqDist;
    return static_cast<u8>(a.depthFill) > static_cast<u8>(b.depthFill);
}

} // namespace whiteout::flakes::renderer::render_detail
