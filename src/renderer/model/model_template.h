#pragma once

#include "../gfx/gfx.h"
#include "assets/texture_asset_manager.h"
#include "particle.h"
#include "render_target.h" // RenderMode
#include "renderer/particle/base/emit_mesh.h"
#include "renderer/particle/base/emitter_desc.h"
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

struct MeshOverlaySource; // render_model.h

struct ModelTemplate {

    struct SharedGeoset {
        i32 geosetId = -1;
        gfx::BufferHandle ib = gfx::BufferHandle::Invalid;
        gfx::BufferHandle unskinnedVb = gfx::BufferHandle::Invalid;

        gfx::BufferHandle unskinnedVb1 = gfx::BufferHandle::Invalid;
        gfx::BufferHandle tangentVb = gfx::BufferHandle::Invalid;
        /// Standalone TEXCOORD1 stream — see GPUGeoset::uv1Vb.
        gfx::BufferHandle uv1Vb = gfx::BufferHandle::Invalid;
        gfx::BufferHandle boneVb = gfx::BufferHandle::Invalid;
        i32 indexCount = 0;
        i32 vertexCount = 0;
        i32 materialId = -1;
        u32 lod = 0;
        Vector3f localCentroid = {0, 0, 0}; // local bounds center (transparent sort)
        std::shared_ptr<const MeshOverlaySource> overlaySource;
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
    // `.m2` emitters and their descs. A model has these or pe2Configs, never
    // both, so the two share the service's Billboard id space.
    std::vector<M2ParticleEmitterConfig> m2ParticleConfigs;
    std::vector<std::shared_ptr<const particle::EmitterDesc>> m2ParticleDescs;
    std::vector<effects::RibbonEmitterConfig> ribbonConfigs;
    // StarCraft II `RIB_` emitters; a model has these or ribbonConfigs, never
    // both, so the two share the ribbon service's emitter id space.
    std::vector<effects::Sc2RibbonEmitterConfig> sc2RibbonConfigs;
    // StarCraft II `PAR_` emitters. Not shared with pe2Configs the way the two
    // ribbon lists share an id space: SC2 never enters the WC3 registration
    // loop, so this is a separate family behind the desc's one selector.
    std::vector<effects::Sc2ParticleEmitterConfig> sc2ParticleConfigs;
    // The surface `PAR_` shape 7 is born on, built on first spawn of a model
    // that has one and shared by every actor of this template — the same
    // once-and-share rule as pe2Descs. Null when no emitter uses the shape,
    // which is the overwhelming majority; the bool is what stops a model with
    // no usable triangles from retrying the build per actor.
    std::shared_ptr<const particle::EmitMesh> sc2EmitMesh;
    bool sc2EmitMeshTried = false;
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
