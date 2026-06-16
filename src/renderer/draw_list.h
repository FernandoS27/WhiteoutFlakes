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

// One entry in the unified back-to-front transparent queue (WC3's
// IModelRenderSceneTransparent). `kind` selects which producer draws it and
// `unit` indexes into that producer's per-unit list. Sorting is producer-
// agnostic: priorityPlane ascending, then camera distance back-to-front.
enum class TransparentKind : u8 { Geoset = 0, Particle = 1, Ribbon = 2, Corn = 3 };

struct TransparentDraw {
    f32 sqDist = 0.0f;
    i32 priorityPlane = 0;
    TransparentKind kind = TransparentKind::Geoset;
    u32 unit = 0;
};

inline bool TransparentDrawOrder(const TransparentDraw& a, const TransparentDraw& b) {
    if (a.priorityPlane != b.priorityPlane)
        return a.priorityPlane < b.priorityPlane;
    if (a.sqDist != b.sqDist)
        return a.sqDist > b.sqDist; // farther first (back-to-front)
    if (a.kind != b.kind)
        return static_cast<u8>(a.kind) < static_cast<u8>(b.kind);
    return a.unit < b.unit;
}

} // namespace whiteout::flakes::renderer::render_detail
