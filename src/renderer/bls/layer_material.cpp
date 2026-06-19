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
        // Fading opaque layer promoted to a blend so its sub‑1.0 alpha actually
        // fades it. WC3's SelectModelMaterial promotes the blend mode whenever
        // the layer alpha drops below full — for the HD Color twin (depth laid
        // down by its Depth twin) AND the plain SD pass. The SD body of e.g.
        // Kil'jaeden is filter Transparent at alpha 0.76 and must blend, not
        // render solid.
        if (isOpaqueFading && m.alpha < GxMatAlpha::Blend)
            m.alpha = GxMatAlpha::Blend;
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
