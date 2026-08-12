#pragma once

// Draw-list value types + sort comparators for the geoset passes. Pure data and
// pure ordering — no GPU, no behaviour — so the comparators are unit-testable
// and the collection (BuildDrawLists), classification (ClassifyGeoset) and
// submission (DrawLayer) stages communicate only through these POD items.
//
// Ordering mirrors WC3 (ModelRender.cpp): opaque is a state-batching sort;
// transparent is back-to-front by camera distance with the Depth prepass twin
// drawn just before its Color draw.
//
// Opaque order is NOT pixel-neutral, despite the depth buffer. Measured with
// gate G2 (REFACTOR_PLAN.md §2): reversing the actor walk — a pure opaque
// reordering — moved pixels in 5 of the 17 corpus models, from 1 byte on
// Goblin_Combine_HD up to 18645 of 1048576 on nightelf_exp. Coplanar opaque
// surfaces resolve by submit order under a LessEqual depth test, so "the depth
// buffer sorts it" is a batching rationale, not a correctness guarantee. Treat
// any change to these comparators as a pixel change and gate it on G2.

#include "bls/layer_material.h" // bls::DepthFill
#include "core/surface_vocabulary.h"
#include "whiteout/flakes/types.h"

#include <vector>

namespace whiteout::flakes::renderer::render_detail {

struct RenderableView;

// One draw: a whole geoset (all its visible layers, drawn in layer order so a
// base layer precedes any additive detail on top). Drawing whole geosets
// matches per-layer splitting visually and fits both the SD and the HD
// (internal-prepass) submission paths.
//
// One struct for both lists. The old OpaqueItem / TransparentItem already
// shared `view` + `geoIdx`; the transparent one's extra fields are the
// superset, and an opaque item leaves them at their defaults — which is why
// unifying them changes no ordering: OpaqueOrder never looked at them.
//
// `depthFill` stays None on the SD path — SD never depth-fills and HD does its
// fade internally; the field is the seam for a WC3-exact HD Color/Depth split.
// `key` is unpopulated until P3a fills the surface tables; nothing reads it yet.
struct DrawItem {
    const RenderableView* view = nullptr;
    i32 geoIdx = -1;
    core::SurfaceKey key;
    bls::DepthFill depthFill = bls::DepthFill::None;
    f32 sqDist = 0.0f; // squared camera distance (back-to-front key)
    i32 priorityPlane = 0;
    bool underWater = false;
};

struct DrawLists {
    std::vector<DrawItem> opaque;
    std::vector<DrawItem> transparent;
};

// Opaque order: a per-model grouping. `view` points into the frame's `views`
// vector, which BuildDrawLists fills in ascending actor-handle order, so this
// is a strict total order keyed on the scene rather than on container layout.
// (WC3 additionally batches by texture/material to cut state changes; that's a
// perf optimization we can layer on later.)
inline bool OpaqueOrder(const DrawItem& a, const DrawItem& b) {
    if (a.view != b.view)
        return a.view < b.view;
    return a.geoIdx < b.geoIdx;
}

// Transparent order mirrors CTransparentObject::HasHigherPriority: underwater
// first, priorityPlane ascending, distance back-to-front, then the Depth
// prepass twin (2) before its Color draw (1) at the same position.
//
// The trailing (view, geoIdx) term is not cosmetic. The depth-buffer argument
// that lets the opaque list be loosely ordered covers only the opaque list —
// two transparent geosets tying on all four keys (duplicate actors at the same
// position, or two geosets sharing a centroid) reorder freely, and
// back-to-front blend order does change pixels. With the term this is a strict
// total order, which is also what makes stable_sort provably a no-op here.
inline bool TransparentOrder(const DrawItem& a, const DrawItem& b) {
    if (a.underWater != b.underWater)
        return a.underWater;
    if (a.priorityPlane != b.priorityPlane)
        return a.priorityPlane < b.priorityPlane;
    if (a.sqDist != b.sqDist)
        return a.sqDist > b.sqDist;
    if (a.depthFill != b.depthFill)
        return static_cast<u8>(a.depthFill) > static_cast<u8>(b.depthFill);
    if (a.view != b.view)
        return a.view < b.view;
    return a.geoIdx < b.geoIdx;
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
