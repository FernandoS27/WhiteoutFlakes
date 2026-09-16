#include "renderer/model/render_model.h"

#include "renderer/profiles/wc3/wc3_surface_table.h"

#include <algorithm>

namespace whiteout::flakes::renderer::model {

using namespace ::whiteout::flakes::renderer::effects;

void RenderModel::ApplyGeosetStates(const FrameState& state) {
    for (i32 i = 0; i < (i32)state.geosetTransforms.size() && i < (i32)gpuGeosets.size(); i++)
        gpuGeosets[i].worldMatrix = state.geosetTransforms[i];

    for (i32 i = 0; i < (i32)state.geosetAlphas.size() && i < (i32)gpuGeosets.size(); i++)
        gpuGeosets[i].geosetAlpha = state.geosetAlphas[i];

    for (i32 i = 0; i < (i32)state.geosetHidden.size() && i < (i32)gpuGeosets.size(); i++)
        gpuGeosets[i].hidden = state.geosetHidden[i] != 0;

    for (i32 i = 0; i < (i32)state.geosetColors.size() && i < (i32)gpuGeosets.size(); i++)
        gpuGeosets[i].geosetColor = state.geosetColors[i];

    for (i32 i = 0; i < (i32)state.geosetDyes.size() && i < (i32)gpuGeosets.size(); i++)
        gpuGeosets[i].dye = state.geosetDyes[i];
}

void RenderModel::ApplyLayerStates(const FrameState& state) {

    // Sized to the highest surface the source reported, not to the table:
    // a source may animate fewer surfaces than the table holds, and a reader
    // falls back to the table's constant for anything past the end.
    i32 maxSurface = -1;
    for (const auto& ss : state.surfaceStates)
        maxSurface = std::max(maxSurface, ss.surface);
    surfaceAnim.assign(static_cast<usize>(maxSurface + 1), SurfaceAnim{});
    for (const auto& ss : state.surfaceStates) {
        if (ss.surface < 0 || ss.surface > maxSurface)
            continue;
        auto& e = surfaceAnim[static_cast<usize>(ss.surface)];
        e.color = ss.color;
        e.alpha = ss.alpha;
        for (i32 k = 0; k < 4; ++k)
            e.unitWeights[k] = ss.unitWeights[k];
    }

    matTexAnim.clear();
    for (auto& ta : state.texAnims) {
        i32 key = ta.materialId * 1000 + ta.layerIndex;
        matTexAnim[key] = {ta.uOff, ta.vOff, ta.uTile, ta.vTile, ta.rotation};
    }

    i32 maxTexAnimId = -1;
    for (auto& tam : state.texAnimMatrices) {
        if (tam.textureAnimId > maxTexAnimId)
            maxTexAnimId = tam.textureAnimId;
    }
    texAnimPalette.assign(std::max(0, maxTexAnimId + 1),
                          TexAnimPaletteEntry{{1.0f, 0.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f, 0.0f}});
    for (auto& tam : state.texAnimMatrices) {
        if (tam.textureAnimId < 0 || tam.textureAnimId > maxTexAnimId)
            continue;
        auto& e = texAnimPalette[tam.textureAnimId];
        for (i32 k = 0; k < 4; ++k) {
            e.row0[k] = tam.row0[k];
            e.row1[k] = tam.row1[k];
        }
    }

    i32 maxMapAlphaId = -1;
    for (const auto& ma : state.layerMapAlphas)
        maxMapAlphaId = std::max(maxMapAlphaId, ma.layerId);
    layerMapAlphaPalette.assign(static_cast<usize>(maxMapAlphaId + 1), -1.0f);
    for (const auto& ma : state.layerMapAlphas) {
        if (ma.layerId >= 0)
            layerMapAlphaPalette[static_cast<usize>(ma.layerId)] = ma.alpha;
    }

    // The animated half of the material. It lives in the table because the
    // submission path reads it from there, but it is rewritten every frame —
    // which is why Wc3SurfaceTable is mutable rather than build-once. An actor
    // whose table hasn't been built yet has nothing to animate; the empty
    // stand-in keeps the loops below bounds-checked without a branch each.
    static const std::vector<GPUMaterial> kNoMaterials;
    // Only Warcraft III keeps its animated material state in the surface table.
    // An `.m2` or `.m3` actor has its own product's table here, so asking for
    // WC3's is a product-boundary crossing rather than the benign "no table
    // yet" case — check the product first, which is the contract
    // ISurfaceTable::Product documents. Casting unconditionally tripped
    // SurfaceTableCast's assert on every M3/M2 actor from the frame their
    // surface tables started being built; the release path was already null
    // here, so nothing about what gets drawn changes.
    auto* table =
        (surfaceTable && surfaceTable->Product() == profiles::wc3::Wc3SurfaceTable::kProduct)
            ? core::SurfaceTableCast<profiles::wc3::Wc3SurfaceTable>(surfaceTable.get())
            : nullptr;
    auto& gpuMaterials =
        table ? table->Materials() : const_cast<std::vector<GPUMaterial>&>(kNoMaterials);
    for (auto& la : state.layerAlphas) {
        if (la.materialId >= 0 && la.materialId < (i32)gpuMaterials.size()) {
            auto& layers = gpuMaterials[la.materialId].cpu.layers;
            if (la.layerIndex >= 0 && la.layerIndex < (i32)layers.size())
                layers[la.layerIndex].alpha = la.alpha;
        }
    }

    for (auto& lt : state.layerTextureIds) {
        if (lt.materialId < 0 || lt.materialId >= (i32)gpuMaterials.size())
            continue;
        auto& layers = gpuMaterials[lt.materialId].cpu.layers;
        if (lt.layerIndex < 0 || lt.layerIndex >= (i32)layers.size())
            continue;
        auto& L = layers[lt.layerIndex];
        switch (lt.slot) {
        case FrameState::LayerTexSlot::Diffuse:
            L.textureId = lt.textureId;
            break;
        case FrameState::LayerTexSlot::Normal:
            L.normalMapId = lt.textureId;
            break;
        case FrameState::LayerTexSlot::ORM:
            L.ormMapId = lt.textureId;
            break;
        case FrameState::LayerTexSlot::Emissive:
            L.emissiveMapId = lt.textureId;
            break;
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
                L.fresnelColor = lf.fresnelColor;
                L.fresnelOpacity = lf.fresnelOpacity;
                L.fresnelTeamColor = lf.fresnelTeamColor;
                L.emissiveGain = lf.emissiveGain;
            }
        }
    }

    activeLights = state.lights;
}

std::shared_ptr<const MeshOverlaySource> MakeMeshOverlaySource(const MeshData& mesh) {
    auto src = std::make_shared<MeshOverlaySource>();
    src->positions = mesh.positions;
    src->indices = mesh.indices;

    // `.m3` carries its weights inside the record, so no bone stream will be
    // built for the overlay to re-pack from. Only the byte formats the adapters
    // write are read; anything else leaves the overlay rigid.
    if (!mesh.baked.Valid())
        return src;
    const VertexAttribute* weights = nullptr;
    const VertexAttribute* indices = nullptr;
    for (const auto& a : mesh.baked.attributes) {
        if (a.semantic == VertexSemantic::BoneWeights && a.semanticIndex == 0)
            weights = &a;
        else if (a.semantic == VertexSemantic::BoneIndices && a.semanticIndex == 0)
            indices = &a;
    }
    if (!weights || !indices || weights->format != gfx::Format::R8G8B8A8_UNORM ||
        indices->format != gfx::Format::R8G8B8A8_UINT)
        return src;
    const u32 count = mesh.baked.VertexCount();
    const u32 stride = mesh.baked.stride;
    if (weights->offset + 4u > stride || indices->offset + 4u > stride)
        return src;
    src->boneWeights.resize(static_cast<usize>(count) * 4);
    src->boneIndices.resize(static_cast<usize>(count) * 4);
    for (u32 v = 0; v < count; ++v) {
        const u8* record = mesh.baked.data.data() + static_cast<usize>(v) * stride;
        std::copy_n(record + weights->offset, 4, src->boneWeights.data() + v * 4);
        std::copy_n(record + indices->offset, 4, src->boneIndices.data() + v * 4);
    }
    return src;
}

} // namespace whiteout::flakes::renderer::model
