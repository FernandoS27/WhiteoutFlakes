#pragma once

#include "common_types.h"
#include "gfx/gfx.h"
#include "renderer/model_instance.h"
#include "renderer/types.h"

#include <memory>
#include <unordered_map>
#include <vector>

namespace WhiteoutDex::bls { struct FrameInputs; }

namespace WhiteoutDex { class SamplerAssetManager; }

namespace WhiteoutDex::render_detail {

struct UnpackedLayer {
    i32      filterMode          = FILTER_NONE;
    i32      flags               = 0;
    f32      alpha               = 1.0f;
    i32      textureId           = -1;
    i32      textureAnimationId  = -1;
    i32      shaderId            = 0;
    i32      normalMapId         = -1;
    i32      ormMapId            = -1;
    i32      emissiveMapId       = -1;
    i32      teamColorMapId      = -1;
    f32      emissiveGain        = 0.0f;
    f32      fresnelOpacity      = 0.0f;
    f32      fresnelTeamColor    = 0.0f;
    Vector3f fresnelColor        = {0.0f, 0.0f, 0.0f};

    i32      coordId             = 0;
};

UnpackedLayer UnpackLayer(const GPUMaterial* mat, i32 layerIndex);

struct RenderableView {
    const std::vector<GPUGeoset>*           geosets        = nullptr;
    const std::vector<GPUMaterial>*         materials      = nullptr;
    TextureAssetManager::ModelScope*        textures       = nullptr;
    const SkinningSystem*                   skinning       = nullptr;
    const std::vector<FrameState::LightState>*       activeLights   = nullptr;
    const std::vector<RenderModel::TexAnimPaletteEntry>* texAnimPalette = nullptr;
    Matrix44f                               worldTransform = Matrix44f::identity();
    f32                                     parentVisibility = 1.0f;
    bool                                    hasLods        = false;
};

struct GeosetRef {
    const RenderableView* view;
    i32 idx;
    i32 renderOrder;
    i32 priorityPlane;
    i32 geosetId;
};

struct CollectedRenderables {
    std::vector<RenderableView> views;
    std::vector<GeosetRef>      refs;
};

CollectedRenderables CollectSortedRenderables(
    const std::unordered_map<u32, std::unique_ptr<Actor>>& models,
    i32 selectedLod);

bool BindSdMeshGeometry(gfx::IGFXCommandList* cmd,
                        const GPUGeoset&      geo,
                        i32                   coordId = 0);

gfx::BufferHandle PickSlot0Vb(const GPUGeoset& geo, i32 coordId);

void BindLayerAlbedo(gfx::IGFXCommandList*       cmd,
                     TextureAssetManager::ModelScope* scope,
                     i32                         textureId,
                     gfx::TextureHandle          defaultTex,
                     SamplerAssetManager&        samplers,
                     u32                         slot = 0);

struct CbPerFrameDesc {
    Matrix44f world         = Matrix44f::identity();
    Matrix44f view          = Matrix44f::identity();
    Matrix44f projection    = Matrix44f::identity();
    Vector4f  lightDir      = {0.0f, 0.0f, 0.0f, 0.0f};
    Vector4f  lightColor    = {1.0f, 1.0f, 1.0f, 1.0f};
    Vector4f  ambientColor  = {1.0f, 1.0f, 1.0f, 0.0f};
    Vector4f  extraParams   = {1.0f, 1.0f, 1.0f, 1.0f};
    Vector4f  texAnimParams = {0.0f, 0.0f, 1.0f, 1.0f};
    Vector4f  materialFlags = {0.0f, 0.0f, 0.0f, 0.0f};
};

void WriteCbPerFrame(gfx::IGFXDevice*     gfx,
                     gfx::BufferHandle    cb,
                     const CbPerFrameDesc& d);

Vector4f NormalizedLightDir4(const Vector4f& dir);

void ApplyTexAnimPaletteToFrame(bls::FrameInputs&    frame,
                                const std::vector<RenderModel::TexAnimPaletteEntry>* palette,
                                i32                  textureAnimationId);

}
