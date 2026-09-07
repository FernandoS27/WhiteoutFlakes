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

// The profile axis, for the two WEM entry points below. A small header — an
// enum, a descriptor struct and a registry lookup — and the alternative is a
// host naming a profile by integer.
#include <whiteout/models/wem/profile.h>
#if WDX_ENABLE_M2
// Included rather than forward-declared: SpawnWowSkinnedModels takes its
// nested SkinnedModel by reference, which needs the definition.
#include "renderer/profiles/wow/wow_character_appearance.h"
#endif
#if WDX_ENABLE_D3
#include "io/d3/d3_item_registry.h"
#include "io/d3/d3_sno_cache.h"
// Included rather than forward-declared for WowCharacterAppearance's reason:
// the accessor hands out a reference and callers reach through it.
#include "renderer/profiles/diablo3/d3_character_appearance.h"
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
class Emitter2;
} // namespace whiteout::flakes::renderer::particle
namespace whiteout::flakes::io {
class IContentProvider;
class M2ModelAdapter;
class D3ModelAdapter;
struct WemDocument;
} // namespace whiteout::flakes::io
namespace whiteout::flakes::renderer::profiles::wow {
class WowReplaceableTextures;
} // namespace whiteout::flakes::renderer::profiles::wow
namespace whiteout::flakes::renderer::profiles::sc2_heroes {
class Sc2ModelCatalog;
} // namespace whiteout::flakes::renderer::profiles::sc2_heroes

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
    Actor* SpawnUnit(const ContentRef& ref, const Matrix44f& initialTm = Matrix44f::identity());

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

    // Non-MDX route: reads @p ref once, sniffs the magic, and spawns through
    // SpawnUnitFromSource when it recognises `.wem`, `.m2` or `.m3`. Null for
    // anything else — including every MDX — so SpawnUnit falls through to the
    // path route untouched.
    //
    // WEM is tested first and unconditionally: it is not one of the three
    // formats but a container that becomes one, and its module is in
    // whiteout_lib whatever this build enables. It opens at the document's own
    // default profile, since nothing here can ask.
    Actor* TrySpawnForeign(const ContentRef& ref,
                           const Matrix44f& initialTm = Matrix44f::identity());

    // Spawn one top-level actor from a `.wem`, as @p profile.
    //
    // The profile is an INPUT because a document can carry a material set per
    // profile over one geometry, so "open this file" has as many answers as it
    // has profiles (WEM_INTEGRATION_DESIGN.md §3). `ProfileId::Count` means
    // "decide" — `io::DefaultWemProfile`'s answer — which is what the paths
    // with no user in front of them use: the CLI, a drop on the window, and
    // TrySpawnForeign's own WEM arm.
    //
    // Sets the scene's product from the profile before anything is staged: the
    // product selects the storage every texture reference resolves against, so
    // a WoW model opened into a Warcraft III scene loses every texture.
    Actor* SpawnWem(
        const ContentRef& ref,
        ::whiteout::models::wem::ProfileId profile = ::whiteout::models::wem::ProfileId::Count,
        const Matrix44f& initialTm = Matrix44f::identity());

    // The same, for a document a host has already parsed — which every host
    // with a profile dialog has, because it had to read the file to know what
    // to offer. Re-reading and re-parsing to spawn would do that work twice.
    Actor* SpawnWemDocument(
        const io::WemDocument& document,
        ::whiteout::models::wem::ProfileId profile = ::whiteout::models::wem::ProfileId::Count,
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
    Actor* SpawnChildFromSource(Actor& parent, ActorRole role, std::shared_ptr<IModelSource> source,
                                u32 forceHandle = 0);

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

#if WDX_ENABLE_M3
    // What names a StarCraft II / Heroes model's external animation files. The
    // same idea as WowReplaceables one format over: an `.m3` carries no path to
    // its `.m3a`, so the answer comes from the game's own catalog and not from
    // the model. Lives here because the spawn path is its only caller.
    //
    // Hosts reach it to prewarm it off the draw thread (the catalog is ~5,700
    // GameData files) and to report what a model was given.
    profiles::sc2_heroes::Sc2ModelCatalog& Sc2Catalog();
#endif

#if WDX_ENABLE_D3
    // Every parsed Diablo III SNO asset, shared across actors. Lives here for
    // the same reason WowReplaceables does — the spawn path is its only caller
    // — and lazily created, because a session that never opens a `.acr` should
    // not pay for the map.
    //
    // 19,154 actors reference 8,550 distinct appearances and the most-shared is
    // named by 594 of them, so this is what stops one 4.5 MB `.app` being read
    // and parsed 594 times. See D3SnoCache for the four decisions behind it.
    io::D3SnoCache& D3Cache();

    // Drawable templates, keyed on `(appearanceSno, lookIndex)` and NOT on the
    // `.acr`. An actor contributes nothing to a drawable beyond those two
    // numbers plus its AnimSet, which rides the actor rather than the template
    // — so keying on the file that was asked for would build 594 separate
    // ModelTemplates off one correctly-shared parse.
    /// @brief Get-or-insert: returns the drawable already built for @p fresh's
    ///        `(appearanceSno, lookIndex)`, or records and returns @p fresh.
    std::shared_ptr<io::D3ModelAdapter> D3Drawable(
        const std::shared_ptr<io::D3ModelAdapter>& fresh);

    // What a player character is wearing. WowCharacters()' sibling and lazily
    // created for the same reason.
    profiles::diablo3::D3CharacterAppearance& D3Characters();

    // The item registry the dressing room equips from: the GameBalance Items
    // tables plus a per-item classification read off each item's Actor.
    // Built lazily on first ask, against the active provider.
    io::D3ItemRegistry& D3Items();

    /// @brief Make @p actorHandle's equipment children match its outfit:
    ///        spawn what is newly equipped, despawn what left, and MOVE a
    ///        child whose hardpoint changed (the sheathe toggle) rather than
    ///        respawning it. Runs inside RestyleD3Model, so every outfit
    ///        change syncs; callable on its own for a sheathe-only change.
    void SyncD3Equipment(u32 actorHandle);

    /// @brief The Diablo III adapter @p actorHandle draws through, or null.
    ///
    /// The outfit is addressed by the *appearance*, not by the file that was
    /// asked for, so a host that wants to dress what it is looking at needs the
    /// adapter rather than a path — see D3CharacterAppearance's keying note.
    std::shared_ptr<io::D3ModelAdapter> D3AdapterOf(u32 actorHandle);

    /// @brief Re-dress @p actorHandle in place after a wardrobe change.
    ///
    /// Returns false when the actor is gone or is not a D3 character, which is
    /// the host's cue to fall back to a reload. Nothing about a restyle needs
    /// one: which geosets draw is frame state, and the materials are a surface
    /// table rebuild plus a re-stage — the same shape as RestyleWowModel, for
    /// the same reason (the document's pose and the camera's framing are not a
    /// function of what the character is wearing).
    bool RestyleD3Model(u32 actorHandle);

    /// @brief The `.acr` @p snoActor, spawned as a child of @p parent riding
    ///        one of its bones.
    ///
    /// A group 1 TriggerEvent payload: another whole model with its own
    /// Appearance and AnimSet. Null when the actor does not resolve or the
    /// tree is already @ref kMaxD3AttachDepth deep. See
    /// `io/d3/d3_effect_resolver.h` and `renderer/effects/d3_attachment_pool.h`.
    /// @brief One child of a `.prt` whose particles ARE models — eSystemType
    ///        1, 3 and 4, 4,795 of the 21,593 shipped files.
    ///
    /// Unlike @ref SpawnD3ChildActor this does not ride a bone: the engine
    /// spawns a free ACD at a world transform and never pushes another one to
    /// it, so the transform the emitter reported is the whole placement.
    Actor* SpawnD3ParticleActor(Actor& owner, i32 snoActor, const Matrix44f& initialTm,
                                u32 forceHandle);

    Actor* SpawnD3ChildActor(Actor& parent, i32 snoActor, i32 bone, const Matrix44f& offset);

    /// @brief Everything a Diablo III actor needs after its Actor exists: the
    ///        surface table, the effects its spawn message starts, and the
    ///        keyframed-attachment pool.
    ///
    /// Shared by the top-level load and by @ref SpawnD3ChildActor, because a
    /// child `.acr` is an actor like any other and has its own effects.
    void SetupD3Actor(Actor& actor, const std::shared_ptr<io::D3ModelAdapter>& d3);
#endif

private:
    // The per-format half of a spawn: the surface table, the surface list and
    // the shading model, chosen by what @p source actually is.
    //
    // Shared by the file path and the WEM path, because a `.m2` that came out
    // of a `.wem` and one that came out of a `.m2` are the same actor — the
    // whole point of converting to the native model rather than teaching the
    // render path a second vocabulary (WEM_INTEGRATION_DESIGN.md §1).
    //
    // A Warcraft III source is left alone: its table is created on first use by
    // Wc3TableFor and its shading model is the scene's default, so stamping
    // Unlit on it here would take a WC3 model off the WC3 path.
    void FinishNativeActor(Actor& actor, const std::shared_ptr<IModelSource>& source);

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
    /// Push a re-resolved skin's particle colours into the emitters ALREADY
    /// running on @p handle.
    ///
    /// A restyle happens in place — the actor keeps its particles, the way the
    /// client's own `ReplaceParticleColor` writes into a live emitter — so the
    /// new colours have to reach the emitter rather than the next spawn.
    /// Without this a skin change moved every texture and left the fire the
    /// colour of the skin the model was opened with.
    void RestyleWowParticleColors(u32 handle, io::M2ModelAdapter& m2);
    /// Register one `.m2` emitter, choosing the emitter class and the id space
    /// from the desc's output. Shared by the template and direct-source paths so
    /// "which kind of emitter is this" is decided once.
    void AddM2Emitter(u32 handle, i32 index, std::shared_ptr<const particle::EmitterDesc> desc,
                      const core::ParticleBehavior& behavior);
    /// The geometry model behind @p key, parsed once and shared. Null (and
    /// remembered as null) when nothing resolves it.
    std::shared_ptr<io::M2ModelAdapter> ResolveParticleModel(const std::string& key);
    /// Parse the geometry model and hold its texture slots on the owning actor,
    /// so the first particle birth is not the first time an asset is asked for.
    void PreloadModelParticleGeometry(u32 handle, const std::string& key);
    /// Resolve an emitter's RPID model and hand its emitters to @p parent as
    /// trails, staging their textures on the owning actor at reserved ids.
    /// See M2_TRAIL_EMITTER_DESIGN.md.
    void AttachTrailEmitters(u32 handle, i32 index, particle::Emitter2& parent,
                             const std::string& key, const core::ParticleBehavior& behavior);

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
    void PreloadChildTemplates(Actor& a, const std::vector<PE1EmitterConfig>& pe1Cfgs,
                               const std::vector<AttachmentConfig>& attachCfgs);

    RenderService& rs_;
#if WDX_ENABLE_D3
    std::unique_ptr<io::D3SnoCache> d3Cache_;
    std::unique_ptr<io::D3ItemRegistry> d3Items_;
    /// One equipment child the outfit put on an actor. Keyed per focus actor:
    /// the outfit is shared per appearance (the documented tradeoff), the
    /// children are real scene actors and cannot be.
    struct D3EquipChild {
        i32 visualSlot = 0;
        i32 itemGbid = -1;
        i32 actorSno = -1;
        std::string hardpoint;
        u32 child = 0;
    };
    std::unordered_map<u32, std::vector<D3EquipChild>> d3Equipment_;
    // (appearanceSno << 8) | lookIndex -> the adapter built for it. A look
    // index above 255 does not exist in shipped content; the census tops out
    // at eight. Weak: an entry decides whether two actors *share* a drawable,
    // and pinning one after its last actor died would keep a 4.5 MB parse alive
    // outside the cache that budgets those.
    std::unordered_map<u64, std::weak_ptr<io::D3ModelAdapter>> d3Drawables_;
    std::unique_ptr<profiles::diablo3::D3CharacterAppearance> d3Characters_;
#endif
#if WDX_ENABLE_M2
    std::unique_ptr<profiles::wow::WowReplaceableTextures> wowReplaceables_;
    std::unique_ptr<profiles::wow::WowCharacterAppearance> wowCharacters_;
    // Geometry models for M2 model particles, by `EmitterDesc::childModelPath`.
    // A null entry is a remembered failure: an emitter births every frame, and
    // re-reading a model that is not there would re-read it every frame.
    std::unordered_map<std::string, std::shared_ptr<io::M2ModelAdapter>> particleModels_;
#endif
#if WDX_ENABLE_M3
    std::unique_ptr<profiles::sc2_heroes::Sc2ModelCatalog> sc2Catalog_;
#endif
};

} // namespace whiteout::flakes::renderer::model
