#pragma once

#include "../gfx/gfx.h"
#include "assets/texture_asset_manager.h"
#include "effects/ribbon.h"
#include "particle.h"
#include "particle/emitter_desc.h"
#include "render_target.h" // RenderMode
#include "whiteout/flakes/model_source.h" // IModelSource, ModelBounds
#include "whiteout/flakes/model_types.h"
#include "whiteout/flakes/types.h"

#include <memory>
#include <string>
#include <vector>


namespace whiteout::flakes::renderer::animation {
struct SkinningData;
}

namespace whiteout::flakes::renderer::model {

struct ModelTemplate {

    struct SharedGeoset {
        i32 geosetId = -1;
        gfx::BufferHandle ib = gfx::BufferHandle::Invalid;
        gfx::BufferHandle unskinnedVb = gfx::BufferHandle::Invalid;

        gfx::BufferHandle unskinnedVb1 = gfx::BufferHandle::Invalid;
        gfx::BufferHandle tangentVb = gfx::BufferHandle::Invalid;
        gfx::BufferHandle boneVb = gfx::BufferHandle::Invalid;
        i32 indexCount = 0;
        i32 vertexCount = 0;
        i32 materialId = -1;
        u32 lod = 0;
        Vector3f localCentroid = {0, 0, 0}; // local bounds center (transparent sort)
    };

    // The source this template was built from, kept alive so per-frame
    // Evaluate() has something to call. Typed as the interface, not as
    // MdxModelAdapter: a template is the renderer's format-neutral snapshot,
    // and every consumer that genuinely needs MDX (Save As, the HD-material
    // probe, per-sequence extents) now downcasts and says so at the call site.
    std::shared_ptr<IModelSource> adapter;

    // What camera framing measures against — see ModelBounds. Filled from the
    // source at build time so nothing has to reach back through `adapter` for
    // it, which is the reason framing used to be MDX-typed.
    ModelBounds bounds;

    std::vector<MeshData> meshes;
    std::vector<TextureData> textures;
    std::vector<MaterialData> materials;
    SkeletonData skeleton;
    std::vector<SkinWeightData> skinWeights;
    std::vector<ParticleEmitterConfig> pe2Configs;
    // Immutable emitter descriptions, built once from pe2Configs on first spawn
    // and shared by every actor of this template. Parallel to pe2Configs.
    std::vector<std::shared_ptr<const particle::EmitterDesc>> pe2Descs;
    std::vector<effects::RibbonEmitterConfig> ribbonConfigs;
    std::vector<CollisionShapeData> collisionConfigs;
    std::vector<PE1EmitterConfig> pe1Configs;
    std::vector<std::shared_ptr<const particle::EmitterDesc>> pe1Descs;
    std::vector<CornEmitterInit> cornEmitterInits;
    std::vector<AttachmentConfig> attachmentConfigs;
    std::vector<EventObjectConfig> eventObjects;
    std::vector<u32> globalSequences;
    std::vector<CameraPreset> cameraPresets;

    std::shared_ptr<animation::SkinningData> skinningData;

    bool gpuUploaded = false;
    std::vector<SharedGeoset> sharedGeosets;

    // Device pointer remembered from uploadTemplateGpu so ~ModelTemplate
    // can auto-release the shared GPU buffers when the last actor's
    // sourceTemplate strong-ref drops. Without this, weak-cache eviction
    // leaks BufferHandle slot indices — the gfx layer eventually reuses
    // them, the queue submits work against stale resources, and
    // WebGPU reports "A valid external Instance reference no longer
    // exists" on the next frame's OnSubmittedWorkDone.
    gfx::IGFXDevice* gpuDevice = nullptr;

    ModelTemplate();
    ~ModelTemplate();
    ModelTemplate(const ModelTemplate&) = delete;
    ModelTemplate& operator=(const ModelTemplate&) = delete;

    void ReleaseGPU(gfx::IGFXDevice& gfx);

    // HD if any layer uses a non-zero BLS shaderId. A hint the host may act on
    // by setting RenderSettings' mode — the renderer never auto-flips on load,
    // and since P5 the mode only chooses between the two WC3 profiles at all.
    // Meaningless for a non-WC3 template, which has no BLS layers and so
    // always reports SD.
    RenderMode PreferredRenderMode() const {
        for (const auto& mat : materials)
            for (const auto& layer : mat.layers)
                if (layer.shaderId != 0)
                    return RenderMode::HD;
        return RenderMode::SD;
    }
};

} // namespace whiteout::flakes::renderer::model
