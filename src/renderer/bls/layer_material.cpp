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
        // Fading opaque layer promoted to a blend so it fades against the
        // depth laid down by its Depth twin.
        if (isOpaqueFading && m.alpha < GxMatAlpha::Blend)
            m.alpha = GxMatAlpha::Blend;
        [[fallthrough]];

    case DepthFill::None:
    default:
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
