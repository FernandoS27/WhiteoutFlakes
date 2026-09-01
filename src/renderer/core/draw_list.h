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
    // Sort ahead of everything else in this priority plane. Set on both halves
    // of an M2 depth-twin pair, standing in for the FLT_MAX that BeginDraw
    // writes into the element's distance-tiebreak slot. A real sentinel float in
    // `sqDist` would collide with the M2 geo zero.
    bool hoist = false;
};

struct DrawLists {
    std::vector<DrawItem> opaque;
    std::vector<DrawItem> transparent;
    // Diablo III's distortion buffer. Not a third blend bucket: the same
    // geometry, drawn a second time into a different render target after the
    // transparent scene. Sorted back-to-front like `transparent`, because the
    // shipped passes blend (SRCALPHA, INVSRCALPHA) into the buffer.
    std::vector<DrawItem> distortion;
};

// Opaque order: group by shading model, then walk the scene. `view` points into
// the frame's `views` vector, which BuildDrawLists fills in ascending
// actor-handle order, so this is a strict total order keyed on the scene rather
// than on container layout. (WC3 additionally batches by texture/material to cut
// state changes; that's a perf optimization we can layer on later.)
//
// `key.model` leads because a model change is the most expensive transition in
// the list — SurfacePass closes one model's pass and opens another's, which
// rebinds every pass-global constant buffer, sampler and probe. Grouping makes
// that O(models) instead of O(draws).
//
// It costs nothing to add while one model is live: an equal leading key leaves
// the comparator exactly as it was, so this is provably order-preserving and
// stays byte-identical. That is why it lands *before* two models coexist rather
// than with them — landing it alongside would make a pixel change and an
// ordering change arrive in one untestable step.
inline bool OpaqueOrder(const DrawItem& a, const DrawItem& b) {
    if (a.key.model != b.key.model)
        return static_cast<u8>(a.key.model) < static_cast<u8>(b.key.model);
    if (a.view != b.view)
        return a.view < b.view;
    if (a.geoIdx != b.geoIdx)
        return a.geoIdx < b.geoIdx;
    // Keeps a multi-surface geoset's draws adjacent and in authored order.
    // Zero for every WC3 item, so the comparator is unchanged for them.
    return a.key.surface < b.key.surface;
}

// Transparent order mirrors CTransparentObject::HasHigherPriority: underwater
// first, priorityPlane ascending, distance back-to-front, then the Depth
// prepass twin (2) before its Color draw (1) at the same position.
//
// WoW reaches the same shape from the other end. CM2Scene::SortTransparent is
// distance, then priorityPlane, then materialLayer — but every `.m2` geo batch
// is given distance 0.0 (only particles and ribbons get a real one), so for
// geometry the distance key ties and the two chains agree. BuildDrawLists feeds
// that same 0.0 rather than branching here; see RenderSettings::
// M2DistanceSortGeometry.
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
    // Depth-writing pairs first, so the plane's depth is laid down before
    // anything blends against it (SortTransparent's FLT_MAX slot).
    if (a.hoist != b.hoist)
        return a.hoist;
    if (a.depthFill != b.depthFill)
        return static_cast<u8>(a.depthFill) > static_cast<u8>(b.depthFill);
    // M2's materialLayer, and it sits ABOVE the actor/geoset terms because
    // SortTransparent does: the client groups every layer-0 batch of a model
    // before every layer-1 batch, rather than finishing one submesh at a time.
    // Zero for every WC3 item, so an equal leading key leaves the WC3
    // comparator exactly as it was — the same argument OpaqueOrder makes for
    // `key.model`.
    if (a.key.sortOrder != b.key.sortOrder)
        return a.key.sortOrder < b.key.sortOrder;
    if (a.view != b.view)
        return a.view < b.view;
    if (a.geoIdx != b.geoIdx)
        return a.geoIdx < b.geoIdx;
    return a.key.surface < b.key.surface;
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
