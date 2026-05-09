#include "bls_mat_params.h"

namespace whiteout::flakes::renderer::bls {

using namespace ::whiteout::flakes::renderer::model;
using namespace ::whiteout::flakes::renderer::particle;

GxMatAlpha FilterToGxAlpha(i32 filterMode) {
    switch (filterMode) {
        case FILTER_NONE:        return GxMatAlpha::Opaque;
        case FILTER_TRANSPARENT: return GxMatAlpha::AlphaKey;
        case FILTER_BLEND:       return GxMatAlpha::Blend;
        case FILTER_ADDITIVE:
        case FILTER_ADD_ALPHA:   return GxMatAlpha::Add;
        case FILTER_MODULATE:    return GxMatAlpha::Modulate;
        case FILTER_MODULATE_2X: return GxMatAlpha::Modulate2X;
        default:                 return GxMatAlpha::Opaque;
    }
}

u32 FilterToDisables(i32 filterMode) {
    switch (filterMode) {
        case FILTER_NONE:        return 0;
        case FILTER_TRANSPARENT: return 0;
        case FILTER_BLEND:       return kDisableDepthWrite;
        case FILTER_ADDITIVE:
        case FILTER_ADD_ALPHA:
        case FILTER_MODULATE:
        case FILTER_MODULATE_2X: return kDisableFog | kDisableDepthWrite;
        default:                 return 0;
    }
}

MatParams FromMdxLayer(i32 filterMode, i32 matFlags, GxShaderID shaderId) {
    MatParams p;
    p.shaderId = shaderId;
    p.alpha    = FilterToGxAlpha(filterMode);
    p.disables = FilterToDisables(filterMode);

    if (matFlags & MAT_TWO_SIDED)     p.disables |= kDisableCull;
    if (matFlags & MAT_UNSHADED)      p.disables |= kDisableLighting;
    if (matFlags & MAT_UNFOGGED)      p.disables |= kDisableFog;
    if (matFlags & MAT_NO_DEPTH_TEST) p.disables |= kDisableDepthTest;
    if (matFlags & MAT_NO_DEPTH_SET)  p.disables |= kDisableDepthWrite;

    p.disables &= ~kDisableBit5;
    return p;
}

MatParams FromParticleDesc(const particle::ParticleMaterialDesc& desc,
                           GxShaderID shaderId) {
    MatParams p;
    p.shaderId = shaderId;
    switch (desc.filterMode) {
        case particle::FilterMode::Blend:
            p.alpha    = GxMatAlpha::Blend;
            p.disables = kDisableDepthWrite;
            break;
        case particle::FilterMode::Additive:
            p.alpha    = GxMatAlpha::Add;
            p.disables = kDisableFog | kDisableDepthWrite;
            break;
        case particle::FilterMode::Modulate:
            p.alpha    = GxMatAlpha::Modulate;
            p.disables = kDisableFog | kDisableDepthWrite;
            break;
        case particle::FilterMode::Modulate2X:
            p.alpha    = GxMatAlpha::Modulate2X;
            p.disables = kDisableFog | kDisableDepthWrite;
            break;
        case particle::FilterMode::AlphaKey:
            p.alpha    = GxMatAlpha::AlphaKey;
            p.disables = 0;
            break;
    }
    if (desc.unshaded) p.disables |= kDisableLighting;
    if (desc.unfogged) p.disables |= kDisableFog;

    p.disables |= kDisableCull;
    return p;
}

}
