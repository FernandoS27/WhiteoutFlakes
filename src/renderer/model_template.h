#pragma once

#include "../gfx/gfx.h"
#include "common_types.h"
#include "model_types.h"
#include "particle.h"
#include "ribbon.h"
#include "texture_asset_manager.h"

#include <memory>
#include <string>
#include <vector>

namespace WhiteoutDex {

class MdxModelAdapter;
struct SkinningData;

struct ModelTemplate {

    struct SharedGeoset {
        i32               geosetId    = -1;
        gfx::BufferHandle ib          = gfx::BufferHandle::Invalid;
        gfx::BufferHandle unskinnedVb = gfx::BufferHandle::Invalid;

        gfx::BufferHandle unskinnedVb1 = gfx::BufferHandle::Invalid;
        gfx::BufferHandle tangentVb   = gfx::BufferHandle::Invalid;
        gfx::BufferHandle boneVb      = gfx::BufferHandle::Invalid;
        i32               indexCount  = 0;
        i32               vertexCount = 0;
        i32               materialId  = -1;
        u32               lod         = 0;
    };

    std::shared_ptr<MdxModelAdapter>   adapter;

    std::vector<MeshData>              meshes;
    std::vector<TextureData>           textures;
    std::vector<MaterialData>          materials;
    SkeletonData                       skeleton;
    std::vector<SkinWeightData>        skinWeights;
    std::vector<ParticleEmitterConfig> pe2Configs;
    std::vector<RibbonEmitterConfig>   ribbonConfigs;
    std::vector<CollisionShapeData>    collisionConfigs;
    std::vector<PE1EmitterConfig>      pe1Configs;
    std::vector<AttachmentConfig>      attachmentConfigs;
    std::vector<EventObjectConfig>     eventObjects;
    std::vector<u32>                   globalSequences;
    std::vector<CameraPreset>          cameraPresets;

    std::shared_ptr<SkinningData>      skinningData;

    bool                               gpuUploaded = false;
    std::vector<SharedGeoset>          sharedGeosets;

    std::unique_ptr<TextureAssetManager::ModelScope> templateTextures;

    ModelTemplate();
    ~ModelTemplate();
    ModelTemplate(const ModelTemplate&)            = delete;
    ModelTemplate& operator=(const ModelTemplate&) = delete;

    void ReleaseGPU(gfx::IGFXDevice& gfx);
};

}
