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
#include "whiteout/flakes/content_ref.h"
#include "whiteout/flakes/model_source.h"
#include "whiteout/flakes/model_types.h"
#include "whiteout/flakes/types.h"
#if WDX_ENABLE_M2
// Included rather than forward-declared: SpawnWowSkinnedModels takes its
// nested SkinnedModel by reference, which needs the definition.
#include "renderer/profiles/wow/wow_character_appearance.h"
#endif

#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace whiteout::flakes::renderer {
class RenderService;
class FrameTicker;
} // namespace whiteout::flakes::renderer
namespace whiteout::flakes::renderer::effects {
class SpnSpawner;
}
namespace whiteout::flakes::renderer::core {
struct ParticleBehavior;
}
namespace whiteout::flakes::renderer::particle {
struct EmitterDesc;
}
namespace whiteout::flakes::io {
class IContentProvider;
class M2ModelAdapter;
} // namespace whiteout::flakes::io
namespace whiteout::flakes::renderer::profiles::wow {
class WowReplaceableTextures;
} // namespace whiteout::flakes::renderer::profiles::wow

namespace whiteout::flakes::renderer::model {

struct Actor;
struct ModelTemplate;
enum class ActorRole : u8;

class ModelLoader {
public:
    explicit ModelLoader(RenderService& rs);
    ~ModelLoader();

    // Spawn one top-level Unit actor from a model reference. Additive — does
    // not clear or touch any other actor. Returns the new Actor* (caller owns
    // the pointer's lifetime via DestroyActor / Clear). The actor's role
    // defaults to ActorRole::Unit; the caller mutates fields like teamColor,
    // playbackSpeed, and animation.SetActiveSequenceIndex on the returned
    // pointer to compose the scene.
    //
    // Dispatches on the *content*, not the name: an `.m2` or `.m3` (by path or
    // by fileDataID) goes through TrySpawnForeign below, and everything else
    // takes the MDX path route. A fileDataID has no extension to branch on,
    // which is why detection is by chunk magic.
    //
    // An id-addressed ref that is neither still returns null:
    // ModelTemplateManager keys its cache on a string and picks MDX vs MDL by
    // extension, so it cannot look one up. Generalising that cache is only
    // worth doing when a second path-addressed format needs it.
    Actor* SpawnUnit(const ContentRef& ref,
                     const Matrix44f& initialTm = Matrix44f::identity());

    // Path convenience. Every host reaches this one — a viewer, a plugin and
    // a thumbnail grid all name models by path and always will.
    Actor* SpawnUnit(const std::string& mdxPath,
                     const Matrix44f& initialTm = Matrix44f::identity()) {
        return SpawnUnit(ContentRef::FromPath(mdxPath), initialTm);
    }

    // Spawn one top-level actor from a live model source (e.g. Max plugin's
    // adapter). Same shape as SpawnUnit; the role defaults to Unit. The Max
    // plugin sets `actor->role = ActorRole::External` after the call so the
    // FrameTicker skips its own evaluation pass.
    // `forceHandle` mirrors SpawnChild's: the particle service mints a handle
    // before the actor exists, and a model particle's `.m2` arrives as a source
    // rather than a template, so this route needs the same door.
    Actor* SpawnUnitFromSource(std::shared_ptr<IModelSource> source,
                               const Matrix44f& initialTm = Matrix44f::identity(),
                               u32 forceHandle = 0);

    // Non-MDX route: reads @p ref once, sniffs the chunk magic, and spawns
    // through SpawnUnitFromSource when it recognises `.m2` or `.m3`. Null for
    // anything else — including every MDX — so SpawnUnit falls through to the
    // path route untouched. Always null with both WDX_ENABLE_M2 and
    // WDX_ENABLE_M3 off.
    Actor* TrySpawnForeign(const ContentRef& ref,
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

    // The same, for a child that arrives as a live IModelSource rather than a
    // ModelTemplate. Attachments, PE1 and SPN all name their children by path
    // and resolve a template; an `.m2` has no template to resolve — the format
    // is parsed straight into an adapter — so it links a SpawnUnitFromSource
    // actor into the tree instead of staging one.
    Actor* SpawnChildFromSource(Actor& parent, ActorRole role,
                                std::shared_ptr<IModelSource> source, u32 forceHandle = 0);

    // One M2 model particle's geometry model, spawned as a PE1-role child of
    // @p owner. @p key is `EmitterDesc::childModelPath` — a path, or `#<id>`
    // when GPID named the model by fileDataID, which every shipped record does.
    //
    // Not routed through the child-TEMPLATE pipeline the MDX side uses: that
    // cache is keyed on a path and always builds an MdxModelAdapter (see the
    // child-model builder in RenderService, where generalising it is P9's job).
    // The `.m2` is instead parsed once per unique key and shared by every
    // particle naming it — `M2ModelAdapter::Evaluate` is const, and its one
    // mutation (a lazily parsed sequence's keys) is idempotent.
    Actor* SpawnModelParticle(Actor& owner, const std::string& key, const Matrix44f& initialTm,
                              u32 forceHandle);

    // Recursively destroy an actor: tears down its children first, releases
    // GPU resources, unregisters from replaceables, removes from the scene
    // actor map, and clears the particle service. Also removes the handle
    // from the parent's `children` list and decrements the PE1 instance
    // counter if role == PE1.
    void DestroyActor(u32 handle);

#if WDX_ENABLE_M2
    // What fills a World of Warcraft model's replaceable texture slots. Lives
    // here because the spawn path is its only caller: an `.m2` actor is built
    // from a freshly parsed model every time, so the slots are resolved per
    // spawn rather than cached on a template. Hosts reach it to offer a skin
    // picker (see WowReplaceableTextures::SetVariation).
    profiles::wow::WowReplaceableTextures& WowReplaceables();

    // The other half of the same idea, for the models the client dresses
    // rather than skins: geoset selection and the composited body texture.
    // Same lifetime and the same reason for living here.
    profiles::wow::WowCharacterAppearance& WowCharacters();

    // Re-dress an already-spawned `.m2` in place: runs the same two passes the
    // spawn does against the actor's own adapter and re-stages its textures.
    //
    // Nothing else about the actor moves — same handle, same sequence, same
    // cursor — so a host can step "Hair Style" without the document reloading
    // underneath the camera. That only works because geoset visibility became
    // frame state (`FrameState::geosetHidden`) and the composited sheets are
    // scope-owned textures the staging path replaces where they stand; neither
    // touches the geometry, which is the same either way.
    //
    // False when the handle is dead or the actor was not spawned from an `.m2`,
    // which is a caller's cue to fall back to a reload.
    bool RestyleWowModel(u32 actorHandle, const ContentRef& ref);
#endif

private:
#if WDX_ENABLE_M2
    // Spawn the collections models @p wanted names as Skinned children of
    // @p character, each showing only the geosets the appearance chose, and
    // wearing the character's own composites. Destroys any it had already.
    //
    // These are where a Dracthyr's horns live: a separate `.m2` posed from the
    // character's skeleton, because the character file declares no geoset for
    // them at all. See profiles::wow::PairBonesByKeyBone.
    void SpawnWowSkinnedModels(
        Actor& character, io::M2ModelAdapter& characterAdapter,
        const std::vector<profiles::wow::WowCharacterAppearance::SkinnedModel>& wanted,
        io::IContentProvider* provider);
#endif

public:

private:
    u32 AddModel(const std::vector<MeshData>& meshes, const std::vector<TextureData>& textures,
                 const std::vector<MaterialData>& materials, const SkeletonData& skeleton,
                 const std::vector<SkinWeightData>& skinWeights,
                 const std::vector<ParticleEmitterConfig>& particleConfigs,
                 const std::vector<effects::RibbonEmitterConfig>& ribbonConfigs,
                 const std::vector<CollisionShapeData>& collisions, u32 forceHandle = 0);
    u32 AddModelByPath(const std::string& mdxPath, const Matrix44f& initialTm);

    // Stage a freshly-allocated Actor against a parsed template — populates
    // staged textures/materials, particle/ribbon/PE1 emitters, and event
    // bindings. The GPU upload is committed in the next CommitPendingUploads()
    // pass. Callers go through SpawnUnit / SpawnUnitFromSource / SpawnChild.
    void StageActor(Actor* mi, std::shared_ptr<ModelTemplate> tmpl);

    // Copy TextureData into an actor's staging map and register any replaceable
    // slot. Shared by the spawn path and the in-place restyle above, which
    // differ only in whether the actor already exists.
    void StageTextures(Actor& mi, const std::vector<TextureData>& textures);

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
    /// `.m2` emitters for the direct-source path. The template path registers
    /// its own; without this one a model loaded straight from an IModelSource
    /// (which is what the headless trace harness does) silently has none.
    void SetM2ParticleConfigs(u32 handle, const std::vector<M2ParticleEmitterConfig>& configs);
    /// Register one `.m2` emitter, choosing the emitter class and the id space
    /// from the desc's output. Shared by the template and direct-source paths so
    /// "which kind of emitter is this" is decided once.
    void AddM2Emitter(u32 handle, i32 index,
                      std::shared_ptr<const particle::EmitterDesc> desc,
                      const core::ParticleBehavior& behavior);
    /// The geometry model behind @p key, parsed once and shared. Null (and
    /// remembered as null) when nothing resolves it.
    std::shared_ptr<io::M2ModelAdapter> ResolveParticleModel(const std::string& key);
    /// Parse the geometry model and hold its texture slots on the owning actor,
    /// so the first particle birth is not the first time an asset is asked for.
    void PreloadModelParticleGeometry(u32 handle, const std::string& key);

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
#if WDX_ENABLE_M2
    std::unique_ptr<profiles::wow::WowReplaceableTextures> wowReplaceables_;
    std::unique_ptr<profiles::wow::WowCharacterAppearance> wowCharacters_;
    // Geometry models for M2 model particles, by `EmitterDesc::childModelPath`.
    // A null entry is a remembered failure: an emitter births every frame, and
    // re-reading a model that is not there would re-read it every frame.
    std::unordered_map<std::string, std::shared_ptr<io::M2ModelAdapter>> particleModels_;
#endif
};

} // namespace whiteout::flakes::renderer::model
