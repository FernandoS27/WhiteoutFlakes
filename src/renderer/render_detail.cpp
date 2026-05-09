#include "render_detail.h"
#include "renderer/render_service.h"
#include "constants.h"
#include "renderer/assets/sampler_asset_manager.h"
#include "bls/bls_frame.h"

#include <algorithm>
#include <cmath>

namespace whiteout::flakes::renderer::render_detail {

using namespace ::whiteout::flakes::renderer::model;
using namespace ::whiteout::flakes::renderer::assets;
using namespace ::whiteout::flakes::renderer::bls;

CollectedRenderables CollectSortedRenderables(
    const std::unordered_map<u32, std::unique_ptr<Actor>>& models,
    i32 selectedLod) {
    CollectedRenderables out;

    out.views.reserve(models.size());
    out.refs.reserve(models.size() * 4);

    for (const auto& [h, miPtr] : models) {
        Actor* mi = miPtr.get();
        if (mi->parentVisibility <= 0.02f) continue;

        RenderableView& view = out.views.emplace_back();
        view.geosets          = &mi->render.gpuGeosets;
        view.materials        = &mi->render.gpuMaterials;
        view.textures         = mi->render.textures.get();
        view.skinning         = &mi->render.skinning;
        view.activeLights     = &mi->render.activeLights;
        view.texAnimPalette   = &mi->render.texAnimPalette;
        view.worldTransform   = mi->worldTransform;
        view.parentVisibility = mi->parentVisibility;
        view.hasLods          = mi->render.hasLods;
        view.teamColor        = mi->teamColor;

        const i32 modelLod = mi->render.hasLods ? selectedLod : 0;
        const i32 geosetCount = static_cast<i32>(mi->render.gpuGeosets.size());
        for (i32 i = 0; i < geosetCount; ++i) {
            const auto& geo = mi->render.gpuGeosets[i];
            if (!GeosetPassesLod(geo.lod, modelLod)) continue;
            i32 ro = 1;
            const i32 matId = geo.materialId;
            if (matId >= 0 && matId < static_cast<i32>(mi->render.gpuMaterials.size())
                && !mi->render.gpuMaterials[matId].cpu.layers.empty()) {
                ro = GeosetRenderOrder(
                    mi->render.gpuMaterials[matId].cpu.layers[0].filterMode);
            }
            out.refs.push_back({&view, i, ro, geo.priorityPlane, geo.geosetId});
        }
    }

    std::sort(out.refs.begin(), out.refs.end(),
        [](const GeosetRef& a, const GeosetRef& b) {
            if (a.renderOrder   != b.renderOrder)   return a.renderOrder   < b.renderOrder;
            if (a.priorityPlane != b.priorityPlane) return a.priorityPlane < b.priorityPlane;
            return a.geosetId < b.geosetId;
        });

    return out;
}

gfx::BufferHandle PickSlot0Vb(const GPUGeoset& geo, i32 coordId) {
    if (coordId == 1 && geo.unskinnedVb1 != gfx::BufferHandle::Invalid)
        return geo.unskinnedVb1;
    return geo.unskinnedVb;
}

bool BindSdMeshGeometry(gfx::IGFXCommandList* cmd,
                        const GPUGeoset&      geo,
                        i32                   coordId) {

    cmd->BindVertexBuffer(0, PickSlot0Vb(geo, coordId), sizeof(Vertex));
    cmd->BindIndexBuffer(geo.ib, gfx::Format::R32_UINT);

    const bool hasBones = (geo.boneVb != gfx::BufferHandle::Invalid)
                       && (geo.bonePaletteCb != gfx::BufferHandle::Invalid);
    if (hasBones) {
        cmd->BindVertexBuffer(1, geo.boneVb, sizeof(BoneVertex));
        cmd->BindConstantBuffer(gfx::ShaderStage::Vertex, 3, geo.bonePaletteCb);
    }
    return hasBones;
}

void BindLayerAlbedo(gfx::IGFXCommandList*            cmd,
                     TextureAssetManager::ModelScope* scope,
                     i32                              textureId,
                     gfx::TextureHandle               defaultTex,
                     SamplerAssetManager&             samplers,
                     u32                              slot) {

    u32 wrapFlags = kSamplerWrapBitsMask;
    bool     hasTex    = false;
    if (textureId >= 0 && scope) {
        const gfx::TextureHandle h = scope->Get(textureId);
        if (h != gfx::TextureHandle::Invalid) {
            cmd->BindShaderResource(gfx::ShaderStage::Pixel, slot, h);
            wrapFlags = scope->WrapFlags(textureId);
            hasTex    = true;
        }
    }
    if (!hasTex) cmd->BindShaderResource(gfx::ShaderStage::Pixel, slot, defaultTex);
    cmd->BindSampler(gfx::ShaderStage::Pixel, slot, samplers.WrapVariant(wrapFlags));
}

void WriteCbPerFrame(gfx::IGFXDevice*      gfx,
                     gfx::BufferHandle     cb,
                     const CbPerFrameDesc& d) {
    if (!gfx || cb == gfx::BufferHandle::Invalid) return;
    auto* p = static_cast<CBPerFrame*>(gfx->MapBuffer(cb));
    if (!p) return;
    p->world         = d.world.transpose();
    p->view          = d.view.transpose();
    p->projection    = d.projection.transpose();
    p->lightDir      = d.lightDir;
    p->lightColor    = d.lightColor;
    p->ambientColor  = d.ambientColor;
    p->extraParams   = d.extraParams;
    p->texAnimParams = d.texAnimParams;
    p->materialFlags = d.materialFlags;
    gfx->UnmapBuffer(cb);
}

Vector4f NormalizedLightDir4(const Vector4f& dir) {
    const f32 n = std::sqrt(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
    if (n <= 1e-6f) return {0.0f, 0.0f, 0.0f, 0.0f};
    return {dir.x / n, dir.y / n, dir.z / n, 0.0f};
}

void ApplyTexAnimPaletteToFrame(bls::FrameInputs&    frame,
                                const std::vector<RenderModel::TexAnimPaletteEntry>* palette,
                                i32                  textureAnimationId) {
    if (palette &&
        textureAnimationId >= 0 &&
        textureAnimationId < static_cast<i32>(palette->size())) {
        const auto& e = (*palette)[textureAnimationId];
        frame.texMtx0.rows[0] = { e.row0[0], e.row0[1], e.row0[2], e.row0[3] };
        frame.texMtx0.rows[1] = { e.row1[0], e.row1[1], e.row1[2], e.row1[3] };
    } else {
        frame.texMtx0 = bls::IdentityTexMtx();
    }
    frame.texMtx1 = bls::IdentityTexMtx();
}

UnpackedLayer UnpackLayer(const GPUMaterial* mat, i32 layerIndex) {
    UnpackedLayer out;
    if (!mat || layerIndex < 0 || layerIndex >= static_cast<i32>(mat->cpu.layers.size())) {
        return out;
    }
    const auto& L = mat->cpu.layers[layerIndex];
    out.filterMode          = L.filterMode;
    out.flags               = L.flags;
    out.alpha               = L.alpha;
    out.textureId           = L.textureId;
    out.textureAnimationId  = L.textureAnimationId;
    out.shaderId            = L.shaderId;
    out.normalMapId         = L.normalMapId;
    out.ormMapId            = L.ormMapId;
    out.emissiveMapId       = L.emissiveMapId;
    out.teamColorMapId      = L.teamColorMapId;
    out.emissiveGain        = L.emissiveGain;
    out.fresnelOpacity      = L.fresnelOpacity;
    out.fresnelTeamColor    = L.fresnelTeamColor;
    out.fresnelColor        = L.fresnelColor;
    out.coordId             = L.coordId;
    return out;
}

}
