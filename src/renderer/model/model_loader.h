#pragma once

// ============================================================================
// ModelLoader — model creation / staging / GPU-upload orchestration.
//
// Public spawn primitives are additive: each call appends one Actor to the
// scene without touching what's already there. Hosts compose multi-unit
// scenes by calling SpawnUnit / SpawnUnitFromSource / SpawnChild repeatedly
// and tracking actor pointers themselves. Use RequestClearAll() to reset
// (asynchronous — the actors get reaped on the next CommitPendingUploads /
// RenderFrame so GPU resource teardown happens on the render thread).
// ============================================================================

#include "types.h" // Matrix44f
#include "whiteout/flakes/model_source.h"
#include "whiteout/flakes/model_types.h"
#include "whiteout/flakes/types.h"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace whiteout::flakes::renderer {
class RenderService;
class FrameTicker;
} // namespace whiteout::flakes::renderer
namespace whiteout::flakes::renderer::effects {
class SpnSpawner;
}

namespace whiteout::flakes::renderer::model {

struct Actor;
struct ModelTemplate;
enum class ActorRole : u8;

class ModelLoader {
public:
    explicit ModelLoader(RenderService& rs);
    ~ModelLoader();

    // Spawn one top-level Unit actor from an MDX path. Additive — does not
    // clear or touch any other actor. Returns the new Actor* (caller owns the
    // pointer's lifetime via DestroyActor / Clear). The actor's role defaults
    // to ActorRole::Unit; the caller mutates fields like teamColor,
    // playbackSpeed, and animation.SetActiveSequenceIndex on the returned
    // pointer to compose the scene.
    Actor* SpawnUnit(const std::string& mdxPath,
                     const Matrix44f& initialTm = Matrix44f::identity());

    // Spawn one top-level actor from a live model source (e.g. Max plugin's
    // adapter). Same shape as SpawnUnit; the role defaults to Unit. The Max
    // plugin sets `actor->role = ActorRole::External` after the call so the
    // FrameTicker skips its own evaluation pass.
    Actor* SpawnUnitFromSource(std::shared_ptr<IModelSource> source,
                               const Matrix44f& initialTm = Matrix44f::identity());

    // Schedule every actor in the scene for destruction at the next
    // CommitPendingUploads() pass (which runs at the start of RenderFrame).
    // Asynchronous: hosts iterating Actors() between this call and the next
    // RenderFrame will still see the old actors.
    void RequestClearAll();
    void UpdateMaterials(u32 handle, const std::vector<MaterialData>& mats,
                         const std::vector<TextureData>& texs);
    void CommitPendingUploads();

    // Spawn a child of `parent` with the given role and template. Allocates
    // a fresh handle unless `forceHandle != 0` (PE1's sim pre-allocates
    // handles for its own particle tracking). The returned actor is staged,
    // inserted into the scene, and linked into `parent.children`. Caller
    // performs role-specific post-tweaks (sequence index, birth time).
    Actor* SpawnChild(Actor& parent, ActorRole role, std::shared_ptr<ModelTemplate> tmpl,
                      const Matrix44f& initialTm = Matrix44f::identity(), u32 forceHandle = 0);

    // Recursively destroy an actor: tears down its children first, releases
    // GPU resources, unregisters from replaceables, removes from the scene
    // actor map, and clears the particle service. Also removes the handle
    // from the parent's `children` list and decrements the PE1 instance
    // counter if role == PE1.
    void DestroyActor(u32 handle);

private:
    u32 AddModel(const std::vector<MeshData>& meshes, const std::vector<TextureData>& textures,
                 const std::vector<MaterialData>& materials, const SkeletonData& skeleton,
                 const std::vector<SkinWeightData>& skinWeights,
                 const std::vector<ParticleEmitterConfig>& particleConfigs,
                 const std::vector<effects::RibbonEmitterConfig>& ribbonConfigs,
                 const std::vector<CollisionShapeData>& collisions);
    u32 AddModelByPath(const std::string& mdxPath, const Matrix44f& initialTm);

    // Stage a freshly-allocated Actor against a parsed template — populates
    // staged textures/materials, particle/ribbon/PE1 emitters, and event
    // bindings. The GPU upload is committed in the next CommitPendingUploads()
    // pass. Callers go through SpawnUnit / SpawnUnitFromSource / SpawnChild.
    void StageActor(Actor* mi, std::shared_ptr<ModelTemplate> tmpl);

public:
    // Upload a template's shared GPU resources (per-geoset vertex/index/
    // tangent/bone buffers, shared textures). Called automatically on
    // first-spawn from UploadStagedGeosets; the renderer can also call
    // it eagerly via this entry point right after Templates().Tick()
    // picks up a newly-loaded template — that way the first PE1 child
    // referencing the template doesn't pay the upload cost mid-frame.
    // Idempotent (no-op when tmpl.gpuUploaded is already true). Public
    // because FrameTicker calls it from the per-frame template-handoff
    // drain; the private uploadTemplateGpu does the actual work.
    void UploadTemplateGpu(ModelTemplate& tmpl) {
        uploadTemplateGpu(tmpl);
    }

private:
    void SetAttachmentConfigs(u32 handle, const std::vector<AttachmentConfig>& configs);
    void SetPE1Configs(u32 handle, const std::vector<PE1EmitterConfig>& configs);

    void uploadTemplateGpu(ModelTemplate& tmpl);
    void UploadStagedTextures(Actor& mi);
    void UploadStagedGeosets(Actor& mi);
    void CreateNodePalette(Actor& mi);

    // Acquire AssetManager slots for every unique PE1 + attachment
    // child MDX the actor's template references, and stash the
    // SlotIds on Actor::assetSlots. The slots outlive frame-to-frame
    // churn; DestroyActor releases them. The slot's first Acquire
    // also surfaces the path on the AssetManager needs queue so the
    // host pump can start fetching ahead of the first Birth /
    // attachment-load that actually needs the template.
    void PreloadChildTemplates(Actor& a, const ModelTemplate& tmpl);
    void PreloadChildTemplates(Actor& a,
                               const std::vector<PE1EmitterConfig>& pe1Cfgs,
                               const std::vector<AttachmentConfig>& attachCfgs);

    RenderService& rs_;
};

} // namespace whiteout::flakes::renderer::model
