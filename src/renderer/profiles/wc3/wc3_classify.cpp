#include "profiles/wc3/wc3_classify.h"

#include "bls/bls_mat_params.h"
#include "core/render_detail.h"
#include "profiles/wc3/wc3_surface_table.h"

#include <algorithm>

namespace whiteout::flakes::renderer::profiles::wc3 {

using render_detail::RenderableView;

namespace {
// A layer/geoset alpha at or below ~1/255 is invisible — matches WC3 testing
// `m_layerAlpha != 0` on the byte‑quantised alpha and our draw‑time skip.
constexpr f32 kAlphaEps = 0.004f;
// Below this an HD opaque layer is "fading" and gets promoted to the
// transparent queue. Matches EmitLayersHd's isOpaqueFading threshold so the
// classify and submit sides agree on which geosets fade.
constexpr f32 kOpaqueFadeAlpha = 0.99f;
} // namespace

GeosetClass ClassifyGeoset(const RenderableView& view, const model::GPUGeoset& geo) {
    GeosetClass c;

    const f32 geoAlpha = geo.geosetAlpha * view.parentVisibility;
    if (geoAlpha <= kAlphaEps)
        return c; // whole geoset faded out — not visible

    // Downcast for now: the rule is still core-side, and it reads WC3 layer
    // data. P3b moves it behind IShadingModel::Classify, where the model
    // already knows its own table and this goes away.
    const auto* table = core::SurfaceTableCast<Wc3SurfaceTable>(view.surfaceTable);
    const model::GPUMaterial* mat = table ? table->Material(geo.materialId) : nullptr;

    // An untextured/material‑less geoset is one implicit opaque layer.
    const i32 numLayers = mat ? std::max<i32>(1, (i32)mat->cpu.layers.size()) : 1;

    // Classify by the first *visible* layer, exactly as WC3's IsOpaque does.
    for (i32 li = 0; li < numLayers; ++li) {
        const UnpackedLayer layer = Wc3SurfaceTable::Layer(mat, li);
        if (layer.alpha <= kAlphaEps)
            continue;
        c.visible = true;
        c.firstVisibleLayer = li;

        const bool opaqueFilter =
            bls::FilterToGxAlpha(layer.filterMode) < bls::GxMatAlpha::Blend;

        // WC3 IsOpaque splits on the first layer's shader: the SD / SD-on-HD
        // path classifies purely by blend mode, but a *true* HD layer whose
        // opacity has dropped below full is promoted to transparent and gets a
        // depth-fill twin (the HD branch returns opaque only when
        // m_layerAlpha == 255). We fold the geoset alpha into the test so the
        // classify side agrees with EmitLayersHd's isOpaqueFading, which fades
        // on the combined alpha. shaderId 1/24 are the HD/Crystal shaders;
        // 0 (SD-on-HD) keeps the SD rule, matching WC3's GxShaderID_SD_ON_HD.
        const bool hdLayer = layer.shaderId == 1 || layer.shaderId == 24;
        const f32 combinedAlpha = geoAlpha * layer.alpha;
        const bool hdFading = hdLayer && opaqueFilter && combinedAlpha < kOpaqueFadeAlpha;

        c.opaque = opaqueFilter && !hdFading;
        c.opaqueFilter = opaqueFilter;
        c.needsDepthFill = hdFading;
        break;
    }
    return c;
}

} // namespace whiteout::flakes::renderer::profiles::wc3
