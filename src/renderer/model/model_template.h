#pragma once

#include "../gfx/gfx.h"
#include "common_types.h"
#include "model/model_types.h"
#include "particle.h"
#include "render_target.h"  // RenderMode
#include "effects/ribbon.h"
#include "assets/texture_asset_manager.h"

#include <memory>
#include <string>
#include <vector>

namespace whiteout::flakes::io { class MdxModelAdapter; }
namespace whiteout::flakes::renderer::animation { struct SkinningData; }

namespace whiteout::flakes::renderer::model {

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

    std::shared_ptr<io::MdxModelAdapter>      adapter;

    std::vector<MeshData>                     meshes;
    std::vector<TextureData>                  textures;
    std::vector<MaterialData>                 materials;
    SkeletonData                              skeleton;
    std::vector<SkinWeightData>               skinWeights;
    std::vector<ParticleEmitterConfig>        pe2Configs;
    std::vector<effects::RibbonEmitterConfig> ribbonConfigs;
    std::vector<CollisionShapeData>           collisionConfigs;
    std::vector<PE1EmitterConfig>             pe1Configs;
    std::vector<AttachmentConfig>             attachmentConfigs;
    std::vector<EventObjectConfig>            eventObjects;
    std::vector<u32>                          globalSequences;
    std::vector<CameraPreset>                 cameraPresets;

    std::shared_ptr<animation::SkinningData>  skinningData;

    bool                                      gpuUploaded = false;
    std::vector<SharedGeoset>                 sharedGeosets;

    std::unique_ptr<assets::TextureAssetManager::ModelScope> templateTextures;

    ModelTemplate();
    ~ModelTemplate();
    ModelTemplate(const ModelTemplate&)            = delete;
    ModelTemplate& operator=(const ModelTemplate&) = delete;

    void ReleaseGPU(gfx::IGFXDevice& gfx);

    // True if any layer uses a non-zero BLS shaderId (HD pipeline). The
    // application uses this to decide which render mode to set on the
    // RenderSettings — the renderer no longer auto-flips on load.
    RenderMode PreferredRenderMode() const {
        for (const auto& mat : materials)
            for (const auto& layer : mat.layers)
                if (layer.shaderId != 0) return RenderMode::HD;
        return RenderMode::SD;
    }
};

}
