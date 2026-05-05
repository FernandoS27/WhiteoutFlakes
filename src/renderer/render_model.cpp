#include "renderer/render_model.h"

#include <algorithm>

namespace WhiteoutDex {

void RenderModel::ApplyGeosetStates(const FrameState& state) {
    for (i32 i = 0; i < (i32)state.geosetTransforms.size() && i < (i32)gpuGeosets.size(); i++)
        gpuGeosets[i].worldMatrix = state.geosetTransforms[i];

    for (i32 i = 0; i < (i32)state.geosetAlphas.size() && i < (i32)gpuGeosets.size(); i++)
        gpuGeosets[i].geosetAlpha = state.geosetAlphas[i];

    for (i32 i = 0; i < (i32)state.geosetColors.size() && i < (i32)gpuGeosets.size(); i++)
        gpuGeosets[i].geosetColor = state.geosetColors[i];
}

void RenderModel::ApplyLayerStates(const FrameState& state) {

    matTexAnim.clear();
    for (auto& ta : state.texAnims) {
        i32 key = ta.materialId * 1000 + ta.layerIndex;
        matTexAnim[key] = {ta.uOff, ta.vOff, ta.uTile, ta.vTile, ta.rotation};
    }

    i32 maxTexAnimId = -1;
    for (auto& tam : state.texAnimMatrices) {
        if (tam.textureAnimId > maxTexAnimId) maxTexAnimId = tam.textureAnimId;
    }
    texAnimPalette.assign(std::max(0, maxTexAnimId + 1),
                          TexAnimPaletteEntry{
                              {1.0f, 0.0f, 0.0f, 0.0f},
                              {0.0f, 1.0f, 0.0f, 0.0f}});
    for (auto& tam : state.texAnimMatrices) {
        if (tam.textureAnimId < 0 || tam.textureAnimId > maxTexAnimId) continue;
        auto& e = texAnimPalette[tam.textureAnimId];
        for (i32 k = 0; k < 4; ++k) { e.row0[k] = tam.row0[k]; e.row1[k] = tam.row1[k]; }
    }

    for (auto& la : state.layerAlphas) {
        if (la.materialId >= 0 && la.materialId < (i32)gpuMaterials.size()) {
            auto& layers = gpuMaterials[la.materialId].cpu.layers;
            if (la.layerIndex >= 0 && la.layerIndex < (i32)layers.size())
                layers[la.layerIndex].alpha = la.alpha;
        }
    }

    for (auto& lt : state.layerTextureIds) {
        if (lt.materialId < 0 || lt.materialId >= (i32)gpuMaterials.size()) continue;
        auto& layers = gpuMaterials[lt.materialId].cpu.layers;
        if (lt.layerIndex < 0 || lt.layerIndex >= (i32)layers.size()) continue;
        auto& L = layers[lt.layerIndex];
        switch (lt.slot) {
            case FrameState::LayerTexSlot::Diffuse:   L.textureId       = lt.textureId; break;
            case FrameState::LayerTexSlot::Normal:    L.normalMapId     = lt.textureId; break;
            case FrameState::LayerTexSlot::ORM:       L.ormMapId        = lt.textureId; break;
            case FrameState::LayerTexSlot::Emissive:  L.emissiveMapId   = lt.textureId; break;
            case FrameState::LayerTexSlot::TeamColor:

                if (L.teamColorMapId != kHdTeamColorActive)
                    L.teamColorMapId = lt.textureId;
                break;
        }
    }

    for (auto& lf : state.layerFresnels) {
        if (lf.materialId >= 0 && lf.materialId < (i32)gpuMaterials.size()) {
            auto& layers = gpuMaterials[lf.materialId].cpu.layers;
            if (lf.layerIndex >= 0 && lf.layerIndex < (i32)layers.size()) {
                auto& L = layers[lf.layerIndex];
                L.fresnelColor     = lf.fresnelColor;
                L.fresnelOpacity   = lf.fresnelOpacity;
                L.fresnelTeamColor = lf.fresnelTeamColor;
                L.emissiveGain     = lf.emissiveGain;
            }
        }
    }

    activeLights = state.lights;
}

void RenderModel::ApplyRibbonFrameStates(const FrameState& state) {
    for (auto& rs : state.ribbonStates) {
        RibbonEmitterState st;
        st.transform   = rs.transform;
        st.above       = rs.above;
        st.below       = rs.below;
        st.alpha       = rs.alpha;
        st.color       = rs.color;
        st.visibility  = rs.visibility;
        st.slot        = rs.slot;
        ribbons.UpdateEmitterState(rs.emitterId, st);
    }
}

void RenderModel::ApplyPE1FrameStates(const FrameState& state) {
    for (auto& ps : state.pe1States) {
        PE1EmitterState st;
        st.transform    = ps.transform;
        st.emissionRate = ps.emissionRate;
        st.speed        = ps.speed;
        st.latitude     = ps.latitude;
        st.longitude    = ps.longitude;
        st.gravity      = ps.gravity;
        st.visibility   = ps.visibility;
        pe1.UpdateEmitterState(ps.emitterId, st);
    }
}

}
