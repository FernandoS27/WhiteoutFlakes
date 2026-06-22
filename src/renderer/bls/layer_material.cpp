#include "bls/layer_material.h"

namespace whiteout::flakes::renderer::bls {

MatParams ResolveLayerMaterial(DepthFill depthFill, const MatParams& base, bool isOpaqueFading,
                               const Vector4f& color) {
    MatParams m = base;

    switch (depthFill) {
    case DepthFill::Depth:
        // Depth‑only prepass: white, blend, depth write ON (clear the disable),
        // colour write OFF. Matches SelectModelMaterial's DEPTHFILL_DEPTH branch
        // (`m_disables &= ~8; m_disables |= 0x100`).
        m.diffuseColor = {1.0f, 1.0f, 1.0f, 1.0f};
        m.alpha = GxMatAlpha::Blend;
        m.disables &= ~kDisableDepthWrite;
        m.disables |= kDisableBit8;
        return m;

    case DepthFill::Color:
    case DepthFill::None:
    default:
        // A fading opaque/alpha-key layer (combined alpha below full) is promoted
        // to a blend so its sub-1.0 opacity actually fades it — for the plain SD
        // pass and the HD Color twin alike. Crucially, an alpha-KEY layer keeps
        // its cutoff after promotion: the clip ref stays at the alpha-key value
        // so the layer still discards fragments below it. That makes a fully
        // opaque alpha-key cutout stay a hard cutout (not promoted), while a
        // faded one blends its survivors AND keeps the cutoff (e.g. Kil'jaeden's
        // 0.76 body reads semi-transparent; the Matrix crosshatch dissolves as
        // its animated opacity crosses the cutoff).
        if (isOpaqueFading && m.alpha < GxMatAlpha::Blend) {
            if (m.alpha == GxMatAlpha::AlphaKey)
                m.alphaRef = kAlphaKeyRef;
            m.alpha = GxMatAlpha::Blend;
        }
        // Modulate packs the alpha into R (the shader reads .r); everything else
        // takes {rgb, combinedAlpha} straight. Mirrors SelectModelMaterial's
        // shared colour‑set tail.
        if (m.alpha == GxMatAlpha::Modulate)
            m.diffuseColor = {color.w, 1.0f, 1.0f, 1.0f};
        else
            m.diffuseColor = color;
        return m;
    }
}

} // namespace whiteout::flakes::renderer::bls
