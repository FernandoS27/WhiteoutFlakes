#include "geoset_classify.h"

#include "bls/bls_mat_params.h"
#include "render_detail.h"

#include <algorithm>

namespace whiteout::flakes::renderer::render_detail {

namespace {
// A layer/geoset alpha at or below ~1/255 is invisible — matches WC3 testing
// `m_layerAlpha != 0` on the byte‑quantised alpha and our draw‑time skip.
constexpr f32 kAlphaEps = 0.004f;
} // namespace

GeosetClass ClassifyGeoset(const RenderableView& view, const model::GPUGeoset& geo) {
    GeosetClass c;

    const f32 geoAlpha = geo.geosetAlpha * view.parentVisibility;
    if (geoAlpha <= kAlphaEps)
        return c; // whole geoset faded out — not visible

    const model::GPUMaterial* mat = nullptr;
    if (geo.materialId >= 0 && geo.materialId < static_cast<i32>(view.materials->size()))
        mat = &(*view.materials)[geo.materialId];

    // An untextured/material‑less geoset is one implicit opaque layer.
    const i32 numLayers = mat ? std::max<i32>(1, (i32)mat->cpu.layers.size()) : 1;

    // Classify by the first *visible* layer, exactly as WC3's IsOpaque does.
    for (i32 li = 0; li < numLayers; ++li) {
        const UnpackedLayer layer = UnpackLayer(mat, li);
        if (layer.alpha <= kAlphaEps)
            continue;
        c.visible = true;
        c.firstVisibleLayer = li;
        c.opaque = bls::FilterToGxAlpha(layer.filterMode) < bls::GxMatAlpha::Blend;
        break;
    }
    return c;
}

} // namespace whiteout::flakes::renderer::render_detail
