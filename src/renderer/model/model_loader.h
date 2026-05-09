#pragma once

// ============================================================================
// ModelLoader — model creation / staging / GPU-upload orchestration extracted
// from RenderService.
//
// Public API matches the historical RenderService entry points used by tools
// (LoadFromMdx, SpawnFromLiveSource, Clear, UpdateMaterials) plus the
// per-frame upload step (CommitPendingUploads, called from
// RenderService::RenderFrame).
//
// The private members are the staging / upload internals that other renderer
// subsystems (FrameTicker, SpnSpawner) reach into via friend access — same
// pattern FrameTicker uses to talk to RenderService::Impl.
// ============================================================================

#include "common_types.h"
#include "model/model_types.h"
#include "model/model_source.h"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace whiteout::flakes::renderer {
    class RenderService;
    class FrameTicker;
}
namespace whiteout::flakes::renderer::effects { class SpnSpawner; }

namespace whiteout::flakes::renderer::model {

struct Actor;
struct ModelTemplate;

class ModelLoader {
public:
    explicit ModelLoader(RenderService& rs);
    ~ModelLoader();

    Actor* LoadFromMdx(const std::string& mdxPath);
    Actor* SpawnFromLiveSource(std::shared_ptr<IModelSource> source);
    void   Clear();
    void   UpdateMaterials(const std::vector<MaterialData>& mats,
                           const std::vector<TextureData>&  texs);
    void   CommitPendingUploads();

    // Stage a freshly-allocated Actor against a parsed template — populates
    // staged textures/materials, particle/ribbon/PE1 emitters, and event
    // bindings. Used by FrameTicker (attachment/PE1 children) and SpnSpawner
    // (.spn children). The actor's GPU upload is committed in the next
    // CommitPendingUploads() pass.
    void   StageActor(Actor* mi, std::shared_ptr<ModelTemplate> tmpl);

private:
    u32  AddModel(const std::vector<MeshData>& meshes,
                  const std::vector<TextureData>& textures,
                  const std::vector<MaterialData>& materials,
                  const SkeletonData& skeleton,
                  const std::vector<SkinWeightData>& skinWeights,
                  const std::vector<ParticleEmitterConfig>& particleConfigs,
                  const std::vector<effects::RibbonEmitterConfig>& ribbonConfigs,
                  const std::vector<CollisionShapeData>& collisions);
    u32  AddModelByPath(const std::string& mdxPath);
    u32  LoadModelByPath(const std::string& mdxPath);

    void SetAttachmentConfigs(u32 handle, const std::vector<AttachmentConfig>& configs);
    void SetPE1Configs(u32 handle, const std::vector<PE1EmitterConfig>& configs);
    void UpdateMaterials(u32 handle,
                         const std::vector<MaterialData>& mats,
                         const std::vector<TextureData>&  texs);
    bool IsTextureCached(std::string_view key) const;

    void uploadTemplateGpu(ModelTemplate& tmpl);
    void UploadStagedTextures(Actor& mi);
    void UploadStagedGeosets(Actor& mi);
    void CreateNodePalette(Actor& mi);

    RenderService& rs_;

    // RenderService installs a texture-cache lambda on the template manager
    // that dispatches to IsTextureCached.
    friend class RenderService;
};

}
