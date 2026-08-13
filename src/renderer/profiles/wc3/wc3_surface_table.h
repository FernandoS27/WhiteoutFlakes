#pragma once

// Wc3SurfaceTable — WC3's material data, held verbatim. `StagedMaterial` /
// `StagedMaterialLayer` are unchanged: this phase moves where the data lives,
// not what it is, which is why UpdateMaterials' public signature survives.
//
// UnpackedLayer moved in here with it. Only the *static* half belongs to the
// table: `alpha`, the texture ids and the fresnel values are written every
// frame by RenderModel::ApplyFrameState from the animation, so what UnpackLayer
// returns is a snapshot of the table's current state, not a load-time constant.

#include "core/surface_table.h"
#include "renderer/model/render_model.h"
#include "whiteout/flakes/model_types.h"
#include "whiteout/flakes/types.h"

#include <vector>

namespace whiteout::flakes::renderer::profiles::wc3 {

// One layer, flattened out of the material for the submission path. Mixes
// static material data with the per-frame animated values the tick wrote back
// into the table.
struct UnpackedLayer {
    i32 filterMode = model::FILTER_NONE;
    i32 flags = 0;
    f32 alpha = 1.0f;
    i32 textureId = -1;
    i32 textureAnimationId = -1;
    i32 shaderId = 0;
    i32 normalMapId = -1;
    i32 ormMapId = -1;
    i32 emissiveMapId = -1;
    i32 teamColorMapId = -1;
    f32 emissiveGain = 0.0f;
    f32 fresnelOpacity = 0.0f;
    f32 fresnelTeamColor = 0.0f;
    Vector3f fresnelColor = {0.0f, 0.0f, 0.0f};

    i32 coordId = 0;
};

class Wc3SurfaceTable final : public core::ISurfaceTable {
public:
    static constexpr core::ProductId kProduct = core::ProductId::Wc3;

    core::ProductId Product() const override {
        return kProduct;
    }
    usize Count() const override {
        return materials_.size();
    }

    /// @brief The material at `surface`, or null when the index is out of
    ///        range — which happens legitimately for a geoset whose material
    ///        has not been uploaded yet.
    const model::GPUMaterial* Material(i32 surface) const {
        if (surface < 0 || surface >= static_cast<i32>(materials_.size()))
            return nullptr;
        return &materials_[surface];
    }

    i32 LayerCount(i32 surface) const {
        const auto* m = Material(surface);
        return m ? static_cast<i32>(m->cpu.layers.size()) : 0;
    }

    // Static: it reads only `mat`, and the callers legitimately hold a null
    // table for an actor whose materials haven't uploaded yet. As a member
    // function this was a null-`this` call — UB the optimizer may assume away,
    // which is exactly what it did.
    static UnpackedLayer Layer(const model::GPUMaterial* mat, i32 layerIndex) {
        UnpackedLayer out;
        if (!mat || layerIndex < 0 || layerIndex >= static_cast<i32>(mat->cpu.layers.size()))
            return out;
        const auto& L = mat->cpu.layers[layerIndex];
        out.filterMode = L.filterMode;
        out.flags = L.flags;
        out.alpha = L.alpha;
        out.textureId = L.textureId;
        out.textureAnimationId = L.textureAnimationId;
        out.shaderId = L.shaderId;
        out.normalMapId = L.normalMapId;
        out.ormMapId = L.ormMapId;
        out.emissiveMapId = L.emissiveMapId;
        out.teamColorMapId = L.teamColorMapId;
        out.emissiveGain = L.emissiveGain;
        out.fresnelOpacity = L.fresnelOpacity;
        out.fresnelTeamColor = L.fresnelTeamColor;
        out.fresnelColor = L.fresnelColor;
        out.coordId = L.coordId;
        return out;
    }

    UnpackedLayer Layer(i32 surface, i32 layerIndex) const {
        return Layer(Material(surface), layerIndex);
    }

    /// @brief Direct access for the two writers: the loader draining
    ///        stagedMaterials, and ApplyFrameState writing the animated
    ///        values back. Both are per-actor and per-frame respectively;
    ///        neither is a load-time-only path, which is why the table is
    ///        mutable rather than immutable-after-build.
    std::vector<model::GPUMaterial>& Materials() {
        return materials_;
    }
    const std::vector<model::GPUMaterial>& Materials() const {
        return materials_;
    }

private:
    std::vector<model::GPUMaterial> materials_;
};

} // namespace whiteout::flakes::renderer::profiles::wc3
