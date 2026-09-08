// ============================================================================
// ModelLoader — extracted from RenderService.
//
// Owns model creation, staging, and per-frame GPU-upload commit. Reaches into
// RenderService::Impl directly via friend access (same pattern used by
// FrameTicker and the other in-tree subsystems — see render_service_impl.h).
// ============================================================================

#include "model/model_loader.h"

#include "../io/mdx_model_adapter.h"
#include "assets/asset_manager.h"
#include "assets/replaceable_texture_manager.h"
#include "assets/sampler_asset_manager.h"
#include "assets/texture_asset_manager.h"
#include "bls/bls_cb_layout.h"
#include "bls/bls_draw_helpers.h"
#include "effects/spn_spawner.h"
#include "io/file_content_provider.h" // FileContentProvider::SetGame
#include "io/wem/wem_import.h"
#include "io/wem/wem_profiles.h"
#include "model/model_instance.h"
#include "model/model_template.h"
#include "model/model_template_manager.h"
#include "particle/child_model_emitter.h"
#include "particle/model_particle_emitter.h"
#include "particle/particle2_emitter.h"
#include "particle/particle_adapters.h"
#include "render_service.h"
#include "render_service_impl.h"
#include "scene_manager.h"

#include "dbg_print.h"

#include "renderer/animation/anim_math.h"
#include "renderer/core/render_profile.h" // IRenderProfile::WorldScale
#if WDX_ENABLE_M3
#include "io/m3/m3_model_adapter.h"
#include "renderer/profiles/sc2_heroes/m3_surface_table.h"
#include "renderer/profiles/sc2_heroes/sc2_model_catalog.h"
#endif
#if WDX_ENABLE_D3
#include "io/d3/d3_effect_resolver.h"
#include "io/d3/d3_model_adapter.h"
#include "io/d3/d3_particle_adapter.h"
#include "renderer/particle/d3_emitter.h"
#include "renderer/profiles/diablo3/d3_particle_shading.h"
#include "renderer/profiles/diablo3/d3_surface_table.h"
#endif
#if WDX_ENABLE_M2
#include <whiteout/models/m2/types.h>
#include "io/m2/m2_model_adapter.h"
#include "renderer/profiles/wow/m2_surface_table.h"
#include "renderer/profiles/wow/wow_character_appearance.h"
#include "renderer/profiles/wow/wow_replaceable_textures.h"
#endif
#include "renderer/profiles/wc3/wc3_surface_table.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace whiteout::flakes::renderer::model {

using namespace ::whiteout::flakes::renderer::animation;
using namespace ::whiteout::flakes::renderer::effects;
using namespace ::whiteout::flakes::renderer::particle;
using namespace ::whiteout::flakes::renderer::assets;
using namespace ::whiteout::flakes::renderer::bls;

// The WEM profile axis. Named the way io/wem/ names it, so the two spellings of
// `ProfileId` in this file — the format's and the scene's ProductId — cannot be
// confused for each other.
namespace wem = ::whiteout::models::wem;
using namespace ::whiteout::flakes::io;

namespace {
// Where a trail emitter's textures are bound on the OWNING actor's scope. A
// trail's record indexes its own model's texture array, so its ids have to live
// somewhere no real texture id can reach; the scope is an id->entry map, so a
// sparse high id costs nothing. Four per parent emitter, the client's cap.
constexpr i32 kTrailTextureIdBase = 0x40000;

// The actor's WC3 table, created on first use. Every WC3 load path funnels
// through here, so an actor never has a null table by the time anything draws.
//
// Null when the actor's table belongs to another product — an `.m2` carries an
// M2SurfaceTable built at spawn. Returning null rather than asserting is the
// point: these call sites run for every actor in the scene, and a foreign one
// simply has no WC3 material to read.
profiles::wc3::Wc3SurfaceTable* Wc3TableFor(RenderModel& render) {
    if (!render.surfaceTable)
        render.surfaceTable = std::make_unique<profiles::wc3::Wc3SurfaceTable>();
    if (render.surfaceTable->Product() != core::ProductId::Wc3)
        return nullptr;
    return static_cast<profiles::wc3::Wc3SurfaceTable*>(render.surfaceTable.get());
}

// The public `flakes::ProductId`, which is what SceneView reports — not the
// renderer-internal twin in core/surface_table.h.
const char* ProductName(::whiteout::flakes::ProductId p) {
    switch (p) {
    case ::whiteout::flakes::ProductId::Wc3:
        return "Warcraft III";
    case ::whiteout::flakes::ProductId::Wow:
        return "World of Warcraft";
    case ::whiteout::flakes::ProductId::Sc2:
        return "StarCraft II";
    case ::whiteout::flakes::ProductId::D3:
        return "Diablo III";
    default:
        return "no product";
    }
}

// Local-space bounds center of a geoset. Used as its sort position in the
// back-to-front transparent pass (transformed by the actor world matrix at
// collection time). `get(i)` returns vertex i's position.
template <class Get>
Vector3f GeosetBoundsCenter(i32 count, Get&& get) {
    if (count <= 0)
        return {0, 0, 0};
    Vector3f lo = get(0), hi = lo;
    for (i32 i = 1; i < count; ++i) {
        const Vector3f p = get(i);
        lo.x = std::min(lo.x, p.x);
        lo.y = std::min(lo.y, p.y);
        lo.z = std::min(lo.z, p.z);
        hi.x = std::max(hi.x, p.x);
        hi.y = std::max(hi.y, p.y);
        hi.z = std::max(hi.z, p.z);
    }
    return {(lo.x + hi.x) * 0.5f, (lo.y + hi.y) * 0.5f, (lo.z + hi.z) * 0.5f};
}
} // namespace

ModelLoader::ModelLoader(RenderService& rs) : rs_(rs) {}
ModelLoader::~ModelLoader() = default;

#if WDX_ENABLE_D3
namespace {
// Defined with the other surface builders further down; declared here because
// RestyleD3Model needs it and sits with the rest of the D3 plumbing.
void BuildD3Surfaces(Actor& actor);
} // namespace

io::D3SnoCache& ModelLoader::D3Cache() {
    if (!d3Cache_)
        d3Cache_ = std::make_unique<io::D3SnoCache>(rs_.Scene().ActiveContentProvider());
    return *d3Cache_;
}

std::shared_ptr<io::D3ModelAdapter> ModelLoader::D3Drawable(
    const std::shared_ptr<io::D3ModelAdapter>& fresh) {
    if (!fresh || fresh->AppearanceSno() <= 0)
        return fresh;
    const u64 key = (static_cast<u64>(static_cast<u32>(fresh->AppearanceSno())) << 8) |
                    (fresh->LookIndex() & 0xFFu);
    if (auto it = d3Drawables_.find(key); it != d3Drawables_.end()) {
        auto held = it->second.lock();
        // A host that moved the look on a shared adapter has invalidated the
        // key it was filed under; drop the stale entry rather than handing back
        // a drawable that no longer matches what was asked for.
        if (held && held->LookIndex() == fresh->LookIndex())
            return held;
        d3Drawables_.erase(it);
    }
    // Weak, so an entry does not pin a 4.5 MB parse after every actor using it
    // is gone. The parse itself stays in D3SnoCache, which is budgeted; this
    // map only decides whether two actors share one drawable.
    d3Drawables_.emplace(key, fresh);
    return fresh;
}

profiles::diablo3::D3CharacterAppearance& ModelLoader::D3Characters() {
    if (!d3Characters_) {
        d3Characters_ = std::make_unique<profiles::diablo3::D3CharacterAppearance>();
        // The hair cutaway reads tag 0x10404 off the helm's per-class attach
        // model, which is an Actor the appearance discovers it needs mid-equip.
        d3Characters_->SetActorSource([this](i32 sno) { return D3Cache().Actor(sno); });
    }
    return *d3Characters_;
}

io::D3ItemRegistry& ModelLoader::D3Items() {
    if (!d3Items_)
        d3Items_ = std::make_unique<io::D3ItemRegistry>();
    return *d3Items_;
}

std::shared_ptr<io::D3ModelAdapter> ModelLoader::D3AdapterOf(u32 actorHandle) {
    Actor* actor = rs_.Scene().Actors().Find(actorHandle);
    if (!actor)
        return nullptr;
    return std::dynamic_pointer_cast<io::D3ModelAdapter>(actor->animation.Source());
}

bool ModelLoader::RestyleD3Model(u32 actorHandle) {
    Actor* actor = rs_.Scene().Actors().Find(actorHandle);
    if (!actor)
        return false;
    auto d3 = std::dynamic_pointer_cast<io::D3ModelAdapter>(actor->animation.Source());
    if (!d3 || !D3Characters().Apply(*d3))
        return false;

    // Geosets need nothing more — which of them draw is frame state, so the
    // next Evaluate already reports the new set. The materials do: a look is
    // *which variant of its material* a piece wears, and that is a different
    // texture as well as a different colour set.
    auto table = profiles::diablo3::BuildD3SurfaceTable(
        d3->SourceAppearance(), d3->LookIndex(),
        io::CollectD3Textures(d3->SourceAppearance(), d3->LookIndex(), d3->EmittedSubObjects(),
                              d3->GeosetLooks()),
        d3->EmittedSubObjects(), &D3Cache(), d3->GeosetLooks());
    bool anyValid = false;
    for (const auto& sf : table->Surfaces())
        anyValid |= sf.valid;
    if (!anyValid)
        return false;
    actor->render.surfaceTable = std::move(table);
    BuildD3Surfaces(*actor);
    StageTextures(*actor, d3->GetTextures());
    actor->render.stagedDirty = true;
    SyncD3Equipment(actorHandle);
    return true;
}

void ModelLoader::SyncD3Equipment(u32 actorHandle) {
    Actor* actor = rs_.Scene().Actors().Find(actorHandle);
    if (!actor)
        return;
    auto d3 = std::dynamic_pointer_cast<io::D3ModelAdapter>(actor->animation.Source());
    if (!d3)
        return;
    const auto wanted = D3Characters().OutfitAttachments(*d3);
    auto& have = d3Equipment_[actorHandle];

    // Drop entries whose child died some other way (a reload, a Clear), so a
    // stale handle can never despawn an unrelated actor that reused it.
    have.erase(std::remove_if(have.begin(), have.end(),
                              [&](const D3EquipChild& c) {
                                  const Actor* ch = rs_.Scene().Actors().Find(c.child);
                                  return !ch || ch->parent != actorHandle;
                              }),
               have.end());

    const auto& app = d3->SourceAppearance();

    // Match existing children to wanted entries. A shoulder item wants two
    // children under one (slot, gbid) — possibly the same sno — so matching
    // consumes entries: exact hardpoint first, then same-model-anywhere,
    // which is the sheathe toggle and becomes a MOVE, never a respawn.
    std::vector<bool> consumed(wanted.size(), false);
    auto claim = [&](const D3EquipChild& c, bool exactHardpoint) -> i32 {
        for (usize i = 0; i < wanted.size(); ++i) {
            if (consumed[i])
                continue;
            const auto& w = wanted[i];
            if (w.visualSlot != c.visualSlot || w.itemGbid != c.itemGbid ||
                w.actorSno != c.actorSno)
                continue;
            if (exactHardpoint && c.hardpoint != w.hardpoint)
                continue;
            consumed[i] = true;
            return static_cast<i32>(i);
        }
        return -1;
    };
    std::vector<i32> match(have.size(), -1);
    for (usize h = 0; h < have.size(); ++h)
        match[h] = claim(have[h], /*exactHardpoint*/ true);
    for (usize h = 0; h < have.size(); ++h)
        if (match[h] < 0)
            match[h] = claim(have[h], /*exactHardpoint*/ false);

    // The engine stamps an attached item's dye onto every sub-object of the
    // child (ActorModel_AttachItemModel, retail 0x750A70). The child adapter
    // is a SHARED drawable — two dressed players' identical weapons share one
    // — so, like the geoset outfit itself, the dye is per appearance rather
    // than per actor: the documented tradeoff, worn here too.
    auto stampDye = [&](u32 childHandle, i32 dye) {
        Actor* ch = rs_.Scene().Actors().Find(childHandle);
        if (!ch)
            return;
        if (auto ca = std::dynamic_pointer_cast<io::D3ModelAdapter>(ch->animation.Source()))
            ca->SetGeosetDyes(std::vector<i32>(ca->EmittedSubObjects().size(),
                                               (dye >= 2 && dye <= 22) ? dye : 0));
    };

    std::vector<D3EquipChild> kept;
    for (usize h = 0; h < have.size(); ++h) {
        if (match[h] < 0) {
            DestroyActor(have[h].child);
            continue;
        }
        const auto& w = wanted[static_cast<usize>(match[h])];
        stampDye(have[h].child, w.dyeType);
        if (have[h].hardpoint != w.hardpoint) {
            const auto at = io::d3::ResolveD3Attach(app, w.hardpoint);
            if (Actor* ch = rs_.Scene().Actors().Find(have[h].child)) {
                ch->attachParentBone = at.bone;
                ch->attachParentOffset = at.offset;
                ch->worldTransform =
                    BoneRidingTransform(*actor, actor->render.skinning.NodeMatrices(), *ch);
            }
            have[h].hardpoint.assign(w.hardpoint);
        }
        kept.push_back(std::move(have[h]));
    }
    for (usize i = 0; i < wanted.size(); ++i) {
        if (consumed[i])
            continue;
        const auto& w = wanted[i];
        const auto at = io::d3::ResolveD3Attach(app, w.hardpoint);
        Actor* child = SpawnD3ChildActor(*actor, w.actorSno, at.bone, at.offset);
        if (!child)
            continue;
        stampDye(child->handle, w.dyeType);
        D3EquipChild rec;
        rec.visualSlot = w.visualSlot;
        rec.itemGbid = w.itemGbid;
        rec.actorSno = w.actorSno;
        rec.hardpoint.assign(w.hardpoint);
        rec.child = child->handle;
        kept.push_back(std::move(rec));
    }
    have = std::move(kept);
    if (have.empty())
        d3Equipment_.erase(actorHandle);
}

void ModelLoader::SetupD3Actor(Actor& actor, const std::shared_ptr<io::D3ModelAdapter>& d3) {
    // Warm the `dye_ramp` core texture with the actor. D3StandardShading binds
    // it on EVERY D3 draw (the dye term's palette) and would otherwise acquire
    // it on the first one — a draw-time acquire is a mid-capture asset need the
    // dtrace harness rightly aborts on. Same ref resolution as
    // D3StandardShading::DyeRampTexture; the AssetManager dedupes by ref, so
    // the shading model's later Acquire lands on this already-loaded slot.
    {
        ContentRef rampRef = ContentRef::FromPath("Textures/dye_ramp.tex");
        if (auto* provider = rs_.Scene().ActiveContentProvider()) {
            if (const u32 id = provider->FileIdForPath("Base/Textures/dye_ramp.tex"))
                rampRef = ContentRef::FromFileId(id);
        }
        actor.assetSlots.push_back(
            rs_.Assets().Acquire(AssetKind::Texture, assets::kSoleSubKind, rampRef));
    }

    // Built off the parsed appearance, M2's and M3's precedent — per-look
    // material data does not fit MaterialData. Stamped only when something
    // resolved: a table with no valid entry leaves the whole actor on Unlit,
    // which draws where this model would vanish.
    auto table = profiles::diablo3::BuildD3SurfaceTable(
        d3->SourceAppearance(), d3->LookIndex(),
        io::CollectD3Textures(d3->SourceAppearance(), d3->LookIndex(), d3->EmittedSubObjects(),
                              d3->GeosetLooks()),
        d3->EmittedSubObjects(), &D3Cache(), d3->GeosetLooks());
    bool anyValid = false;
    for (const auto& s : table->Surfaces())
        anyValid |= s.valid;
    if (anyValid) {
        actor.render.surfaceTable = std::move(table);
        BuildD3Surfaces(actor);
        actor.shadingModel = core::ShadingModelId::D3Standard;
    }

    // What a TriggerEvent reaches, and it is not only particles.
    //
    // The route that carries the shipped content is a TriggerEvent on the
    // ACTOR, fired by message 1000 when the actor comes into existence and
    // resolving through an EffectGroup more often than not. Its payload is a
    // `.prt` 10,690 times and a whole other `.acr` 594 — a relic on its altar,
    // a banner on its pole — and the second kind becomes a child model here.
    // The third route, `BoneStructure::snoParticle`, is real but rare: 5 of the
    // first 2,500 `.app`, 33 attachments, 3 distinct files. See
    // io/d3/d3_effect_resolver.h for the census that ranks them, and
    // renderer/effects/d3_attachment_pool.h for the fourth, which is per frame.
    //
    // None of them carries per-emitter animation tracks of the kind `.mdx` and
    // `.m2` do. Everything animated lives inside the `.prt`, on the system's
    // own clock, so all the host owes an emitter per frame is the world matrix
    // of the bone (and hardpoint) it rides.
    const auto& app = d3->SourceAppearance();
    i32 emitterId = 0;

    // Emitter shapes 6, 7 and 11 spray off the model's own surface, so they
    // need its triangles. Built at most once per actor and shared by every
    // emitter that asks — 6.7% of the corpus does, and an actor that carries
    // none never pays for it.
    std::shared_ptr<const particle::d3::EmitMesh> emitMesh;
    bool emitMeshTried = false;

    auto addEmitter = [&](i32 snoParticle, i32 bone, const Matrix44f& offset) {
        auto prt = D3Cache().Particle(snoParticle);
        if (!prt)
            return;
        auto desc = io::d3::BuildD3EmitterDesc(*prt, snoParticle);
        profiles::diablo3::D3ResolveParticleMaterial(*prt, &D3Cache(), desc->d3mat);
        profiles::diablo3::D3BindParticleTextures(actor, desc);
        auto em = std::make_unique<particle::d3::Emitter>();
        if (desc->SamplesModelSurface()) {
            if (!emitMeshTried) {
                emitMeshTried = true;
                emitMesh = io::d3::BuildD3EmitMesh(app, d3->EmittedSubObjects());
            }
            em->SetEmitMesh(emitMesh);
        }
        em->SetD3Desc(std::move(desc));
        em->SetAttachBone(bone);
        em->SetAttachOffset(offset);
        // Registered in the space its own output declares: a type 1/3/4 system
        // emits models, and the two spaces are separate id ranges.
        const auto out = em->Desc().output;
        em->SetChildOwner(actor.handle, emitterId, [this] { return rs_.Scene().AllocActorId(); });
        rs_.Particles().AddEmitter(actor.handle, out, emitterId++, std::move(em));
    };

    if (const io::d3n::Actor* acr = d3->SourceActor()) {
        std::string_view look;
        if (d3->LookIndex() < d3->Looks().size())
            look = d3->Looks()[d3->LookIndex()];
        for (const auto& fx :
             io::d3::ResolveActorEffects(*acr, io::d3::kD3MsgActorSpawned, D3Cache(), look)) {
            const auto at = io::d3::ResolveD3Attach(app, fx.hardpoint);
            if (fx.kind == io::d3::ResolvedEffect::Kind::Actor)
                SpawnD3ChildActor(actor, fx.sno, at.bone, at.offset);
            else
                addEmitter(fx.sno, at.bone, at.offset);
        }
    }

    for (usize b = 0; b < app.arBones.size(); ++b)
        if (app.arBones[b].snoParticle.valid())
            addEmitter(app.arBones[b].snoParticle.id, static_cast<i32>(b), Matrix44f::identity());

    // Past every id the load-time route just used, so the two never collide.
    actor.d3Attachments.Bind(d3, &D3Cache(), emitterId,
                             [this] { return rs_.Scene().AllocActorId(); });
}

Actor* ModelLoader::SpawnD3ParticleActor(Actor& owner, i32 snoActor, const Matrix44f& initialTm,
                                         u32 forceHandle) {
    if (owner.treeDepth >= kMaxD3AttachDepth)
        return nullptr;
    auto adapter =
        io::D3ModelAdapter::LoadActorBySno(snoActor, D3Cache(), rs_.Settings().D3LazyAnimations());
    if (!adapter)
        return nullptr;
    adapter = D3Drawable(adapter);

    Actor* child = SpawnChildFromSource(owner, ActorRole::PE1, adapter, forceHandle);
    if (!child)
        return nullptr;
    // A free placement, not a bone ride: `Actor_SpawnFromSno` takes a transform
    // and the system never sends another one.
    child->worldTransform = initialTm;
    child->shadingModel = core::ShadingModelId::Unlit;
    SetupD3Actor(*child, adapter);
    return child;
}

Actor* ModelLoader::SpawnD3ChildActor(Actor& parent, i32 snoActor, i32 bone,
                                      const Matrix44f& offset) {
    if (parent.treeDepth >= kMaxD3AttachDepth)
        return nullptr;
    auto adapter =
        io::D3ModelAdapter::LoadActorBySno(snoActor, D3Cache(), rs_.Settings().D3LazyAnimations());
    if (!adapter)
        return nullptr;
    // Shared on `(appearanceSno, lookIndex)` like any other D3 drawable. A
    // hardpoint child is exactly the kind of model a scene holds several of.
    adapter = D3Drawable(adapter);

    Actor* child = SpawnChildFromSource(parent, ActorRole::Attachment, adapter);
    if (!child)
        return nullptr;
    child->ridesParentBone = true;
    child->attachParentBone = bone;
    child->attachParentOffset = offset;
    child->shadingModel = core::ShadingModelId::Unlit;
    // Placed now, off the parent's live palette, rather than waiting for the
    // next evaluation pass: an anim attachment spawns *after* the walk that
    // would place it, and the frame it appears is a frame it would otherwise
    // spend at the parent's origin.
    child->worldTransform =
        BoneRidingTransform(parent, parent.render.skinning.NodeMatrices(), *child);
    // A spawned `.acr` is an actor like any other: its own materials, its own
    // spawn-message effects, its own keyframed attachments.
    SetupD3Actor(*child, adapter);
    return child;
}
#endif

#if WDX_ENABLE_M2
profiles::wow::WowReplaceableTextures& ModelLoader::WowReplaceables() {
    if (!wowReplaceables_)
        wowReplaceables_ = std::make_unique<profiles::wow::WowReplaceableTextures>();
    return *wowReplaceables_;
}

profiles::wow::WowCharacterAppearance& ModelLoader::WowCharacters() {
    if (!wowCharacters_)
        wowCharacters_ = std::make_unique<profiles::wow::WowCharacterAppearance>();
    return *wowCharacters_;
}
#endif

#if WDX_ENABLE_M3
profiles::sc2_heroes::Sc2ModelCatalog& ModelLoader::Sc2Catalog() {
    if (!sc2Catalog_)
        sc2Catalog_ = std::make_unique<profiles::sc2_heroes::Sc2ModelCatalog>();
    return *sc2Catalog_;
}
#endif

#if WDX_ENABLE_M2

bool ModelLoader::RestyleWowModel(u32 actorHandle, const ContentRef& ref) {
    Actor* actor = rs_.Scene().Actors().Find(actorHandle);
    if (!actor)
        return false;
    auto m2 = std::dynamic_pointer_cast<io::M2ModelAdapter>(actor->animation.Source());
    if (!m2)
        return false;

    // The same pair TrySpawnForeign runs, in the same order and for the same
    // reason — see there.
    auto* provider = rs_.Scene().ActiveContentProvider();
    auto& replaceables = WowReplaceables();
    replaceables.SetContentProvider(provider);
    replaceables.Apply(*m2, ref);
    auto& characters = WowCharacters();
    characters.SetContentProvider(provider);
    std::vector<profiles::wow::WowCharacterAppearance::SkinnedModel> skinned;
    characters.Apply(*m2, ref, &skinned);

    // Geosets need nothing more: which submeshes draw is frame state, so the
    // next Evaluate already reports the new set. The textures do — GetTextures
    // re-reads the slots the two passes just filled, and staging them again
    // makes UploadStagedTextures replace each one where it stands.
    StageTextures(*actor, m2->GetTextures());
    actor->render.stagedDirty = true;
    // Particles do NOT re-read anything: their colours were baked into an
    // EmitterDesc at spawn, so they need the new skin pushed at them.
    RestyleWowParticleColors(actorHandle, *m2);
    // The one part of a restyle that is not in place. A collections model is a
    // different file, and which file is itself a choice.
    SpawnWowSkinnedModels(*actor, *m2, skinned, provider);
    return true;
}

void ModelLoader::RestyleWowParticleColors(u32 handle, io::M2ModelAdapter& m2) {
    const auto& records = m2.SourceModel().particleEmitters;
    // Nothing to do for the 13 596 of 14 060 corpus models that carry no
    // recolourable emitter, and answering that costs a walk over a short array
    // rather than a rebuild of every emitter the actor has.
    const bool any = std::any_of(records.begin(), records.end(), [](const auto& e) {
        return M2ParticleColorOverride::SlotOf(e.particleColorIndex) >= 0;
    });
    if (!any)
        return;

    const auto configs = m2.GetM2ParticleConfigs();
    const bool linear = rs_.Pipeline().LoadTimeProfile().LinearShading();
    const usize n = (std::min)(configs.size(), records.size());
    for (usize i = 0; i < n; ++i) {
        // Only the emitters the skin can actually touch. Re-describing the rest
        // would drop nothing visible but would re-arm their squirt for no
        // reason, and this runs on a user gesture.
        if (M2ParticleColorOverride::SlotOf(records[i].particleColorIndex) < 0)
            continue;
        auto desc = particle::DescFromM2Config(configs[i], linear);
        // SetDesc substitutes a default for anything shapeless, so a desc that
        // did not build would quietly turn a working emitter inert rather than
        // leave it alone. Leaving it alone is the right answer.
        if (!desc || !desc->shape)
            continue;
        // A new desc rather than a new emitter: the pool, the seed and the
        // trails stay exactly as they were, so a skin change recolours the fire
        // instead of putting it out and lighting it again.
        for (const particle::ParticleOutput output :
             {particle::ParticleOutput::Billboard, particle::ParticleOutput::ChildModel}) {
            if (auto* em = rs_.Particles().GetEmitter(handle, output, static_cast<i32>(i))) {
                const bool squirt = em->SquirtPending();
                em->SetDesc(desc);
                em->SetSquirtPending(squirt);
            }
        }
    }
}
#endif

Actor* ModelLoader::SpawnChild(Actor& parent, ActorRole role, std::shared_ptr<ModelTemplate> tmpl,
                               const Matrix44f& initialTm, u32 forceHandle) {
    if (!tmpl)
        return nullptr;

    const u32 childH = forceHandle != 0 ? forceHandle : rs_.Scene().AllocActorId();
    auto child = std::make_unique<Actor>();
    child->handle = childH;
    child->parent = parent.handle;
    child->role = role;
    child->treeDepth = parent.treeDepth + 1;
    child->worldTransform = initialTm;
    child->teamColor = parent.teamColor; // inherit
    if (tmpl->adapter)
        child->animation.Bind(tmpl->adapter);

    // Default birth-time policy by role: Attachment / PE1 children pause with
    // their ancestor (use ancestor's actorTimeMs so paused parents pause their
    // visual children too); SPN children continue using the wall clock since
    // their lifetime is wall-clock-managed by SpnSpawner.
    // Callers may override via animation.SetBirthTimeMs after this call.
    if (role == ActorRole::SPN) {
        child->animation.SetBirthTimeMs(rs_.Scene().GetAnimationTime());
    } else {
        child->animation.SetBirthTimeMs(AncestorActorTimeMs(parent, rs_.Scene().Actors()));
    }

    StageActor(child.get(), tmpl);
    parent.children.push_back(childH);

    Actor* ptr = child.get();
    rs_.Scene().Actors().All()[childH] = std::move(child);
    if (role == ActorRole::PE1)
        rs_.Scene().IncrementPE1Instances();
    return ptr;
}

Actor* ModelLoader::SpawnChildFromSource(Actor& parent, ActorRole role,
                                         std::shared_ptr<IModelSource> source, u32 forceHandle) {
    Actor* child = SpawnUnitFromSource(std::move(source), parent.worldTransform, forceHandle);
    if (!child)
        return nullptr;

    // SpawnUnitFromSource built a top-level actor; this is the linking half of
    // SpawnChild applied on top. Kept as two steps rather than a shared helper
    // because the orders differ — a template child is linked before staging,
    // a source child is built before there is anything to link.
    child->parent = parent.handle;
    child->role = role;
    child->treeDepth = parent.treeDepth + 1;
    child->teamColor = parent.teamColor;
    child->animation.SetBirthTimeMs(AncestorActorTimeMs(parent, rs_.Scene().Actors()));
    parent.children.push_back(child->handle);
    // Paired with DestroyActor's decrement, which keys off the role and not off
    // how the child was spawned. Missing it once model particles took this route
    // would have driven the instance counter negative rather than merely made
    // the cap generous.
    if (role == ActorRole::PE1)
        rs_.Scene().IncrementPE1Instances();
    return child;
}

void ModelLoader::DestroyActor(u32 handle) {
    auto& actors = rs_.Scene().Actors().All();
    auto it = actors.find(handle);
    if (it == actors.end())
        return;

    // Recurse into children first — destroying them while the parent's
    // services are still alive avoids any reentrant lookups.
    // Copy because each DestroyActor call mutates the parent's vector.
    auto childList = it->second->children;
    for (u32 ch : childList)
        DestroyActor(ch);

    it = actors.find(handle);
    if (it == actors.end())
        return;
    Actor& a = *it->second;

    if (a.parent != 0) {
        if (auto* p = rs_.Scene().Actors().Find(a.parent)) {
            auto& cs = p->children;
            cs.erase(std::remove(cs.begin(), cs.end(), handle), cs.end());
        }
    }

    if (a.role == ActorRole::PE1)
        rs_.Scene().DecrementPE1Instances();

    // Release any AssetManager slots the actor holds — both the
    // per-attachment slots populated by UpdateAttachments and the
    // template-prefetched ones in `assetSlots`. Without these
    // releases each model load leaks one ChildModel slot per child
    // path, and (because ChildModel slots own a parsed ModelTemplate
    // with GPU geosets + texture refs) memory grows without bound
    // across model switches.
    for (auto& aslot : a.attachmentSlots) {
        if (aslot.assetSlot != 0) {
            rs_.Assets().Release(aslot.assetSlot);
            aslot.assetSlot = 0;
        }
    }
    for (auto slot : a.assetSlots) {
        if (slot != 0)
            rs_.Assets().Release(slot);
    }
    a.assetSlots.clear();
    rs_.Replaceables().UnregisterModel(a);
    if (rs_.Pipeline().Gfx())
        a.ReleaseGPU(*rs_.Pipeline().Gfx());
    actors.erase(it);
    rs_.Particles().RemoveModel(handle);
    rs_.Ribbons().RemoveModel(handle);
    rs_.CornEffects().RemoveModel(handle);
}

void ModelLoader::RequestClearAll() {
    for (auto& [h, mi] : rs_.Scene().Actors().All()) {
        mi->render.stagedClear = true;
        mi->render.stagedDirty = true;
    }
    rs_.Particles().Clear();
    rs_.Splats().Clear();
    rs_.Spn().Clear();
    rs_.CornEffects().Clear();
}

void ModelLoader::AddM2Emitter(u32 handle, i32 index,
                               std::shared_ptr<const particle::EmitterDesc> desc,
                               const core::ParticleBehavior& behavior) {
    // One M2 emitter is either quads or models, never both, so the index cannot
    // collide across the two id spaces and the seed stays a function of
    // (actor, emitter index) either way.
    const bool models = desc && desc->output == particle::ParticleOutput::ChildModel;
    std::unique_ptr<particle::Emitter2> em;
    if (models) {
        PreloadModelParticleGeometry(handle, desc->childModelPath);
        em = std::make_unique<particle::ModelParticleEmitter>(
            handle, index, [this] { return rs_.Scene().AllocActorId(); });
    } else {
        em = std::make_unique<particle::Emitter2>();
    }
    const std::string trailKey = desc ? desc->trailModelPath : std::string();
    em->SetDesc(std::move(desc));
    em->SetBehavior(behavior);
    em->SetSeed(particle::MixSeed(handle, (u32)index));
    if (!trailKey.empty())
        AttachTrailEmitters(handle, index, *em, trailKey, behavior);
    rs_.Particles().AddEmitter(
        handle, models ? particle::ParticleOutput::ChildModel : particle::ParticleOutput::Billboard,
        index, std::move(em));
}

void ModelLoader::AttachTrailEmitters(u32 handle, i32 index, particle::Emitter2& parent,
                                      const std::string& key,
                                      const core::ParticleBehavior& behavior) {
#if WDX_ENABLE_M2
    auto model = ResolveParticleModel(key);
    auto* a = rs_.Scene().Actors().Find(handle);
    if (!model || !a)
        return;

    const bool linear = rs_.Pipeline().LoadTimeProfile().LinearShading();
    const auto configs = model->GetM2ParticleConfigs();
    const auto textures = model->GetTextures();
    constexpr i32 kMax = static_cast<i32>(particle::Emitter2::kMaxTrails);

    i32 childIdx = 0;
    for (const auto& tcfg : configs) {
        // The client takes min(count, 4) emitters in file order and asserts on
        // the fifth (`RecursiveEmitterModelLoaded` @0x1016a6b60). No shipped
        // recursion model has more than one.
        if (childIdx >= kMax)
            break;
        // Three shapes we do not carry, each dropped rather than half-built:
        // a trail of trails (the client would nest without limit and nothing
        // ships it), a trail whose particles are themselves models (it would
        // need a child-actor channel per trail particle), and a bone generator
        // (it would spawn off the OTHER model's skeleton, which no actor here
        // poses).
        if (!tcfg.recursionModelPath.empty() || !tcfg.geometryModelPath.empty() ||
            tcfg.generator == M2ParticleEmitterConfig::Generator::Bone) {
            std::fprintf(stderr, "[wow] trail emitter %s[%d] unsupported shape, dropped\n",
                         key.c_str(), childIdx);
            ++childIdx;
            continue;
        }

        auto desc =
            std::make_shared<particle::EmitterDesc>(*particle::DescFromM2Config(tcfg, linear));

        // The texture id in a trail's record indexes ITS model's array, which
        // this actor has never staged. Staging it at a reserved id on the
        // owning actor lets the draw path resolve it with no special case: the
        // scope is an id->entry map, so a sparse high id costs nothing.
        if (tcfg.textureId >= 0 && static_cast<usize>(tcfg.textureId) < textures.size()) {
            TextureData td = textures[static_cast<usize>(tcfg.textureId)];
            td.textureId = kTrailTextureIdBase + index * kMax + childIdx;
            desc->material.textureId = td.textureId;
            StageTextures(*a, {td});
            a->render.stagedDirty = true;
        } else {
            desc->material.textureId = -1;
        }

        auto trail = std::make_unique<particle::Emitter2>();
        trail->SetDesc(std::move(desc));
        trail->SetBehavior(behavior);
        // A range no other emitter of this actor occupies, so the stream stays
        // a function of (actor, emitter index, trail index) alone.
        trail->SetSeed(
            particle::MixSeed(handle, 0xC000u + static_cast<u32>(index * kMax + childIdx)));
        trail->SetTrailState(particle::TrailStateFromM2Config(tcfg));
        parent.AddTrail(std::move(trail));
        ++childIdx;
    }
#else
    (void)handle;
    (void)index;
    (void)parent;
    (void)key;
    (void)behavior;
#endif
}

void ModelLoader::SetM2ParticleConfigs(u32 handle,
                                       const std::vector<M2ParticleEmitterConfig>& configs) {
    auto* mi = rs_.Scene().Actors().Find(handle);
    if (!mi)
        return;
    const particle::ParticleBehavior behavior = rs_.Pipeline().LoadTimeProfile().Particles();
    const bool linear = rs_.Pipeline().LoadTimeProfile().LinearShading();
    for (i32 i = 0; i < (i32)configs.size(); i++)
        AddM2Emitter(handle, i, particle::DescFromM2Config(configs[i], linear), behavior);
    if (mi->render.pe2State.size() < configs.size())
        mi->render.pe2State.resize(configs.size());
}

void ModelLoader::SetAttachmentConfigs(u32 handle, const std::vector<AttachmentConfig>& configs) {
    auto* mi = rs_.Scene().Actors().Find(handle);
    if (!mi)
        return;
    mi->attachmentSlots.clear();

    for (auto& cfg : configs) {
        Actor::AttachmentSlot slot;
        slot.config = cfg;
        slot.loaded = cfg.modelPath.empty();
        mi->attachmentSlots.push_back(slot);
    }
}

void ModelLoader::SetPE1Configs(u32 handle, const std::vector<PE1EmitterConfig>& configs) {
    auto* mi = rs_.Scene().Actors().Find(handle);
    if (!mi)
        return;
    for (i32 i = 0; i < (i32)configs.size(); i++) {
        if (configs[i].modelPath.empty())
            continue;
        auto em = std::make_unique<particle::ChildModelEmitter>(
            handle, i, [this] { return rs_.Scene().AllocActorId(); });
        em->SetDesc(particle::DescFromWc3ChildModelConfig(configs[i]));
        em->SetBehavior(rs_.Pipeline().LoadTimeProfile().Particles());
        em->SetSeed(particle::MixSeed(handle, 0x8000u + (u32)i));
        rs_.Particles().AddEmitter(handle, particle::ParticleOutput::ChildModel, i, std::move(em));
    }
}

void ModelLoader::PreloadChildTemplates(Actor& a, const ModelTemplate& tmpl) {
    PreloadChildTemplates(a, tmpl.pe1Configs, tmpl.attachmentConfigs);
    // Corn-fx .pkb / .pkfx — Acquire one Particle slot per unique pkb
    // path the template references. The AssetManager OnApplied hook
    // walks the parsed EffectAssetModel and Acquires the diffuse
    // textures it references, tied to this Particle slot via
    // AddDependency. Everything releases together with the actor.
    std::unordered_set<std::string> seenPkb;
    for (const auto& ce : tmpl.cornEmitterInits) {
        if (ce.pkbPath.empty())
            continue;
        if (!seenPkb.insert(ce.pkbPath).second)
            continue;
        a.assetSlots.push_back(
            rs_.Assets().Acquire(assets::AssetKind::Effect, assets::kSoleSubKind, ce.pkbPath));
    }
}

void ModelLoader::PreloadChildTemplates(Actor& a, const std::vector<PE1EmitterConfig>& pe1Cfgs,
                                        const std::vector<AttachmentConfig>& attachCfgs) {
    // Acquire one ChildModel slot per unique path the actor's template
    // references. The SlotId stays on `a.assetSlots` for the actor's
    // lifetime so the host pump's fetch + parse work is amortized
    // across every birth / attachment-load. DestroyActor releases the
    // refs. Attachment paths share with the per-slot
    // attachmentSlots[i].assetSlot (set later in UpdateAttachments) —
    // both refs point at the same slot via pathToSlot_ dedup, so the
    // refcount math is correct.
    std::unordered_set<std::string> seen;
    auto hold = [&](const std::string& path) {
        if (path.empty())
            return;
        if (!seen.insert(path).second)
            return;
        a.assetSlots.push_back(
            rs_.Assets().Acquire(assets::AssetKind::Model, assets::kSoleSubKind, path));
    };
    for (const auto& cfg : pe1Cfgs)
        hold(cfg.modelPath);
    for (const auto& cfg : attachCfgs)
        hold(cfg.modelPath);
}

void ModelLoader::StageActor(Actor* mi, std::shared_ptr<ModelTemplate> tmpl) {
    if (!tmpl)
        return;

    mi->sourceTemplate = tmpl;

    // Game units → renderer units, stamped once. The value is a property of
    // the profile the scene is running, so it is read here rather than per
    // frame; nothing about it changes while the actor lives.
    //
    // Exactly 1.0 for Warcraft III (pinned by tests/render_profile_test.cpp),
    // and ScaledWorldTransform short-circuits on that, so no WC3 matrix is
    // touched at all.
    mi->worldScale = rs_.Pipeline().ActiveProfile().WorldScale();
    mi->sourceSpace = rs_.Pipeline().ActiveProfile().SourceSpace();
    mi->bounds = tmpl->bounds;

    if (tmpl->adapter)
        mi->animation.Bind(tmpl->adapter);

    for (auto& tex : tmpl->textures) {
        StagedTexture& st = mi->render.stagedTextures[tex.textureId];
        st.width = tex.width;
        st.height = tex.height;
        st.mipLevels = tex.mipLevels;
        st.replaceableId = tex.replaceableId;
        st.wrapFlags = tex.wrapFlags;
        st.cubeMap = tex.cubeMap;
        st.linearData = tex.linearData;
        st.format = tex.format;
        st.sharedKey = tex.sharedKey;
        // Pixels only flow through staging for synthetic textures (no
        // path). File-backed textures route through AssetManager slots
        // via UploadStagedTextures, so their pixels are never staged.
        if (tex.sharedKey.empty())
            st.pixels = tex.pixels;
        if (tex.replaceableId != 0)
            rs_.Replaceables().RegisterModelSlot(*mi, tex.textureId, tex.replaceableId);
    }

    for (auto& mat : tmpl->materials) {
        StagedMaterial& sm = mi->render.stagedMaterials[mat.materialId];
        sm.layers = mat.layers;
        sm.priorityPlane = mat.priorityPlane;
        sm.sortOrder = mat.sortOrder;
    }

    if (tmpl->skinningData && tmpl->skinningData->nodeCount > 0) {
        mi->render.skinning.SetSharedData(tmpl->skinningData);

        mi->render.skinDirty = true;
    }

    // Build the immutable descriptions once per template; every actor spawned
    // from it shares them rather than carrying its own copy of every key,
    // material and sprite-sheet constant.
    if (tmpl->pe2Descs.size() != tmpl->pe2Configs.size()) {
        tmpl->pe2Descs.clear();
        tmpl->pe2Descs.reserve(tmpl->pe2Configs.size());
        for (const auto& pcfg : tmpl->pe2Configs)
            tmpl->pe2Descs.push_back(particle::DescFromWc3Config(pcfg));
    }
    // The dialect is a load-time decision the emitter caches, so it reads the
    // live mode's profile rather than the frame latch — same rule the texture
    // colour space follows.
    const particle::ParticleBehavior particleBehavior =
        rs_.Pipeline().LoadTimeProfile().Particles();
    for (i32 i = 0; i < (i32)tmpl->pe2Configs.size(); i++) {
        auto em = std::make_unique<particle::Emitter2>();
        em->SetDesc(tmpl->pe2Descs[i]);
        em->SetBehavior(particleBehavior);
        // Seed from stable identity, not construction order, so the same scene
        // reproduces its particle motion across runs.
        em->SetSeed(particle::MixSeed(mi->handle, (u32)i));
        rs_.Particles().AddEmitter(mi->handle, particle::ParticleOutput::Billboard, i,
                                   std::move(em));
    }
    // `.m2` emitters register into the same Billboard id space: a model has
    // MDX emitters or M2 ones, never both, so the ids cannot collide.
    if (tmpl->m2ParticleDescs.size() != tmpl->m2ParticleConfigs.size()) {
        const bool linear = rs_.Pipeline().LoadTimeProfile().LinearShading();
        tmpl->m2ParticleDescs.clear();
        tmpl->m2ParticleDescs.reserve(tmpl->m2ParticleConfigs.size());
        for (const auto& mcfg : tmpl->m2ParticleConfigs)
            tmpl->m2ParticleDescs.push_back(particle::DescFromM2Config(mcfg, linear));
    }
    for (i32 i = 0; i < (i32)tmpl->m2ParticleConfigs.size(); i++)
        AddM2Emitter(mi->handle, i, tmpl->m2ParticleDescs[i], particleBehavior);
    mi->render.pe2State.resize((std::max)(tmpl->pe2Configs.size(), tmpl->m2ParticleConfigs.size()));

    const ribbon::RibbonBehavior ribbonBehavior = rs_.Pipeline().LoadTimeProfile().Ribbons();
    for (i32 i = 0; i < (i32)tmpl->ribbonConfigs.size(); i++)
        rs_.Ribbons().AddEmitter(mi->handle, i, ribbon::DescFromWc3Config(tmpl->ribbonConfigs[i]),
                                 ribbonBehavior);

    // PE1 ("particles that ARE models") registers in the same service as the
    // billboards — same pool, same sim, different output.
    if (tmpl->pe1Descs.size() != tmpl->pe1Configs.size()) {
        tmpl->pe1Descs.clear();
        tmpl->pe1Descs.reserve(tmpl->pe1Configs.size());
        for (const auto& cfg : tmpl->pe1Configs)
            tmpl->pe1Descs.push_back(particle::DescFromWc3ChildModelConfig(cfg));
    }
    for (i32 i = 0; i < (i32)tmpl->pe1Configs.size(); i++) {
        // Emitters with no spawn model still occupy an index (see
        // GetPE1Configs) but have nothing to spawn.
        if (tmpl->pe1Configs[i].modelPath.empty())
            continue;
        auto em = std::make_unique<particle::ChildModelEmitter>(
            mi->handle, i, [this] { return rs_.Scene().AllocActorId(); });
        em->SetDesc(tmpl->pe1Descs[i]);
        em->SetBehavior(particleBehavior);
        em->SetSeed(particle::MixSeed(mi->handle, 0x8000u + (u32)i));
        rs_.Particles().AddEmitter(mi->handle, particle::ParticleOutput::ChildModel, i,
                                   std::move(em));
    }

    // CornFx (CornEmitter) — register one emitter per init in the
    // service's per-(actor, emitterId) map. The emitter Acquires a
    // Particle slot on AssetManager for its .pkb; per-frame state
    // flows through FrameState::cornStates → ApplyCornFrameStates.
    const Vector4f teamRGBA = {
        ((mi->teamColor) & 0xFF) / 255.0f,
        ((mi->teamColor >> 8) & 0xFF) / 255.0f,
        ((mi->teamColor >> 16) & 0xFF) / 255.0f,
        1.0f,
    };
    for (const auto& cinit : tmpl->cornEmitterInits) {
        if (cinit.pkbPath.empty())
            continue;
        auto em = std::make_unique<corn_effects::CornEffectsEmitter>(
            rs_.Assets(), cinit.pkbPath, cinit.animVisibilityGuide, cinit.replaceableId,
            cinit.cornEffectsScaling);
        em->SetEmissionRateMultiplier(cinit.defaultEmissionRate);
        em->SetLifeSpanMultiplier(cinit.defaultLifeSpan);
        em->SetSpeedMultiplier(cinit.defaultSpeed);
        em->SetColor(cinit.defaultColor);
        // Seed Game.TeamColor from the actor's own swatch so the first
        // frame's color matches even before ApplyCornFrameStates runs.
        em->SetReplaceableColor(teamRGBA);
        rs_.CornEffects().AddCornEmitter(mi->handle, cinit.emitterId, std::move(em));
    }

    mi->events.Reset(tmpl->eventObjects, tmpl->globalSequences);

    mi->render.stagedDirty = true;
}

void ModelLoader::UpdateMaterials(u32 handle, const std::vector<MaterialData>& materials,
                                  const std::vector<TextureData>& textures) {
    auto* mi = rs_.Scene().Actors().Find(handle);
    if (!mi)
        return;

    for (auto& tex : textures) {
        StagedTexture& st = mi->render.stagedTextures[tex.textureId];
        st.width = tex.width;
        st.height = tex.height;
        st.mipLevels = tex.mipLevels;
        st.replaceableId = tex.replaceableId;
        st.wrapFlags = tex.wrapFlags;
        st.cubeMap = tex.cubeMap;
        st.linearData = tex.linearData;
        st.format = tex.format;
        st.pixels = tex.pixels;
        st.sharedKey = tex.sharedKey;
        if (tex.replaceableId != 0)
            rs_.Replaceables().RegisterModelSlot(*mi, tex.textureId, tex.replaceableId);
    }

    for (auto& mat : materials) {
        StagedMaterial& sm = mi->render.stagedMaterials[mat.materialId];
        sm.layers = mat.layers;
        sm.priorityPlane = mat.priorityPlane;
        sm.sortOrder = mat.sortOrder;
    }

    mi->render.stagedDirty = true;
}

void ModelLoader::StageTextures(Actor& mi, const std::vector<TextureData>& textures) {
    for (const auto& tex : textures) {
        StagedTexture& st = mi.render.stagedTextures[tex.textureId];
        st.width = tex.width;
        st.height = tex.height;
        st.mipLevels = tex.mipLevels;
        st.replaceableId = tex.replaceableId;
        st.wrapFlags = tex.wrapFlags;
        st.cubeMap = tex.cubeMap;
        st.linearData = tex.linearData;
        st.format = tex.format;
        st.pixels = tex.pixels;
        st.sharedKey = tex.sharedKey;

        if (tex.replaceableId != 0)
            rs_.Replaceables().RegisterModelSlot(mi, tex.textureId, tex.replaceableId);
    }
}

u32 ModelLoader::AddModel(const std::vector<MeshData>& meshes,
                          const std::vector<TextureData>& textures,
                          const std::vector<MaterialData>& materials, const SkeletonData& skeleton,
                          const std::vector<SkinWeightData>& skinWeights,
                          const std::vector<ParticleEmitterConfig>& particleConfigs,
                          const std::vector<RibbonEmitterConfig>& ribbonConfigs,
                          const std::vector<CollisionShapeData>& collisions, u32 forceHandle) {
    u32 handle = (forceHandle != 0) ? forceHandle : rs_.Scene().AllocActorId();
    auto mi = std::make_unique<Actor>();
    mi->handle = handle;
    // Same stamp StageActor applies, because this path never reaches it: a
    // live IModelSource has no ModelTemplate, so SpawnUnitFromSource builds
    // the actor here instead. Missing it left every M2 at scale 1 while the
    // profile said 20 — which renders, plausibly, at a twentieth of the right
    // size, and is exactly the failure a golden cannot distinguish from
    // "loaded fine".
    mi->worldScale = rs_.Pipeline().ActiveProfile().WorldScale();
    mi->sourceSpace = rs_.Pipeline().ActiveProfile().SourceSpace();

    StageTextures(*mi, textures);

    for (auto& mat : materials) {
        StagedMaterial& sm = mi->render.stagedMaterials[mat.materialId];
        sm.layers = mat.layers;
        sm.priorityPlane = mat.priorityPlane;
        sm.sortOrder = mat.sortOrder;
    }

    for (auto& mesh : meshes) {
        StagedGeoset& sg = mi->render.stagedGeosets[mesh.geosetId];
        sg.materialId = mesh.materialId;
        sg.lod = mesh.lod;
        sg.deformable = mesh.deformable;
        i32 vc = (i32)mesh.positions.size();
        // The sort centroid comes off `positions` either way — it is the CPU
        // copy, and the baked path deliberately never decodes its own blob.
        sg.centroid = GeosetBoundsCenter(vc, [&](i32 i) { return mesh.positions[i]; });
        if (mesh.baked.Valid()) {
            // Already a GPU vertex buffer. Uploaded verbatim, so there is
            // nothing to interleave and `vertices` stays empty.
            //
            // Copied, not moved: `meshes` is a const ref and the signature is
            // shared with the MDX path. No worse than what it replaces — the
            // array path allocated and filled a 48-byte `Vertex` per vertex
            // right here, and a baked record is never larger than that.
            sg.baked = mesh.baked;
            sg.bakedVertexCount = (i32)sg.baked.VertexCount();
            sg.indices = mesh.indices;
            continue;
        }
        sg.vertices.resize(vc);
        for (i32 i = 0; i < vc; i++) {
            sg.vertices[i].position = mesh.positions[i];
            sg.vertices[i].normal =
                (i < (i32)mesh.normals.size()) ? mesh.normals[i] : Vector3f{0, 0, 1};
            sg.vertices[i].uv = (i < (i32)mesh.uvs.size()) ? mesh.uvs[i] : Vector2f{0, 0};
            sg.vertices[i].color = {1.0f, 1.0f, 1.0f, 1.0f};
        }
        if ((i32)mesh.tangents.size() == vc)
            sg.tangents = mesh.tangents;
        sg.indices = mesh.indices;
    }

    if (skeleton.nodeCount > 0) {

        std::vector<f32> invBindFlat(skeleton.nodeCount * 16);
        for (i32 i = 0; i < skeleton.nodeCount; i++) {
            memcpy(&invBindFlat[i * 16], &skeleton.inverseBindMatrices[i].data[0][0], 64);
        }
        mi->render.skinning.SetSkeleton(skeleton.nodeCount, invBindFlat.data());
        mi->render.billboardFlags = skeleton.billboardFlags;
        mi->render.nodePivots = skeleton.nodePivots;
        mi->render.nodeParents = skeleton.nodeParents;
        mi->render.skinDirty = true;
    }

    // Same Path A/B decision as the template manager — direct-load
    // path (no shared template) needs the same load-time rewrite so
    // its vertex buffer's boneIdx values are global slot indices when
    // the actor lands on Path A. `skinWeights` is passed as const so
    // we copy into a local that DecidePaletteLayoutAndRewrite can
    // mutate in-place; the rewritten copy then feeds the per-geoset
    // weight + vertex-buffer setup below.
    std::vector<SkinWeightData> rewrittenSkinWeights = skinWeights;
    auto paletteDecision =
        animation::DecidePaletteLayoutAndRewrite(skeleton.nodeCount, rewrittenSkinWeights);
    if (auto data = mi->render.skinning.SharedData()) {
        // SharedData was set by SetSkeleton above (which calls
        // ensureOwnedData internally) — apply the decision to the
        // owned instance.
        data->actorPaletteSize = paletteDecision.actorPaletteSize;
        data->usesPerActorPalette = paletteDecision.usesPerActorPalette;
        data->globalGroupAverages = std::move(paletteDecision.globalGroupAverages);
    }

    for (auto& sw : rewrittenSkinWeights) {
        i32 vc = (i32)sw.influences.size();
        std::vector<i32> boneIdx(vc * 4);
        std::vector<f32> weights(vc * 4);
        for (i32 v = 0; v < vc; v++) {
            for (i32 j = 0; j < 4; j++) {
                boneIdx[v * 4 + j] = sw.influences[v].boneIdx[j];
                weights[v * 4 + j] = sw.influences[v].weight[j];
            }
        }
        mi->render.skinning.SetGeosetWeights(sw.geosetId, vc, boneIdx.data(), weights.data());
        GeosetPaletteLayout layout;
        layout.subsetNodeIndices = sw.subsetNodeIndices;
        layout.groupAverages = sw.groupAverages;
        mi->render.skinning.SetGeosetLayout(sw.geosetId, std::move(layout));
    }
    if (!skinWeights.empty())
        mi->render.skinDirty = true;

    const particle::ParticleBehavior particleBehavior =
        rs_.Pipeline().LoadTimeProfile().Particles();
    for (usize i = 0; i < particleConfigs.size(); i++) {
        const auto& pcfg = particleConfigs[i];
        auto em = std::make_unique<particle::Emitter2>();
        em->SetDesc(particle::DescFromWc3Config(pcfg));
        em->SetBehavior(particleBehavior);
        em->SetSeed(particle::MixSeed(handle, (u32)i));
        rs_.Particles().AddEmitter(handle, particle::ParticleOutput::Billboard, (i32)i,
                                   std::move(em));
    }
    mi->render.pe2State.resize(particleConfigs.size());

    const ribbon::RibbonBehavior ribbonBehavior = rs_.Pipeline().LoadTimeProfile().Ribbons();
    for (usize i = 0; i < ribbonConfigs.size(); i++) {
        rs_.Ribbons().AddEmitter(handle, (i32)i, ribbon::DescFromWc3Config(ribbonConfigs[i]),
                                 ribbonBehavior);
    }

    for (auto& cs : collisions) {
        CollisionShape shape;
        shape.type = cs.type;
        shape.vmin = cs.vertices[0];
        shape.vmax = cs.vertices[1];
        shape.radius = cs.radius;
        shape.pivot = cs.pivot;
        shape.bodyKind = cs.bodyKind;
        shape.bodyIndex = cs.bodyIndex;
        shape.hullPoints = cs.hullPoints;
        shape.hullEdges = cs.hullEdges;
        mi->render.collisionShapes.push_back(shape);
    }

    mi->render.stagedDirty = true;
    rs_.Scene().Actors().All()[handle] = std::move(mi);
    return handle;
}

u32 ModelLoader::AddModelByPath(const std::string& mdxPath, const Matrix44f& initialTm) {
    // Everything that reaches here is an MDX/MDL — SpawnUnit routes `.m2` and
    // `.m3` away first — so this is one of the two places a session commits to
    // Warcraft III content, and therefore to opening its install.
    //
    // Settle the product first, for the same reason TrySpawnForeign settles it
    // before parsing: on a scene that owns its provider, that is what re-points
    // the storage, and everything below — the game data, the template, its
    // textures — reads through it. Leaving a previous `.m2`/`.m3` load's product
    // standing would also hand this model that game's render profile.
    if (rs_.Scene().Product() != ProductId::Wc3) {
        rs_.Scene().SetProduct(ProductId::Wc3);
        rs_.Settings().MarkRenderModeDirty();
    }
    rs_.EnsureWc3GameData();

    auto tmpl = rs_.Scene().Templates().GetOrLoadSync(mdxPath);
    if (!tmpl)
        return 0;

    u32 handle;
    {
        handle = rs_.Scene().AllocActorId();
        auto mi = std::make_unique<Actor>();
        mi->handle = handle;
        mi->worldTransform = initialTm;
        StageActor(mi.get(), tmpl);
        for (auto& cs : tmpl->collisionConfigs) {
            CollisionShape shape;
            shape.type = cs.type;
            shape.vmin = cs.vertices[0];
            shape.vmax = cs.vertices[1];
            shape.radius = cs.radius;
            shape.pivot = cs.pivot;
            // Carried, as the other path does. The two overlays *partition* the shape list on
            // this field, so dropping it files every physics body under Collision Markers and
            // leaves all three physics toggles showing nothing.
            shape.bodyKind = cs.bodyKind;
            shape.bodyIndex = cs.bodyIndex;
            shape.hullPoints = cs.hullPoints;
            shape.hullEdges = cs.hullEdges;
            mi->render.collisionShapes.push_back(shape);
        }
        rs_.Scene().Actors().All()[handle] = std::move(mi);
    }

    if (!tmpl->attachmentConfigs.empty())
        SetAttachmentConfigs(handle, tmpl->attachmentConfigs);

    // Acquire (and hold for the actor's lifetime) one ChildModel slot
    // per unique PE1 / attachment child MDX. The host pump fetches +
    // parses each in the background so Birth events / attachment loads
    // land with a ready template; refs are released in DestroyActor.
    if (auto* a = rs_.Scene().Actors().Find(handle))
        PreloadChildTemplates(*a, *tmpl);

    return handle;
}

#if WDX_ENABLE_M2
namespace {
// 'MD20' / 'MD21' little-endian, the two `.m2` chunk magics.
bool LooksLikeM2(std::span<const u8> bytes) {
    if (bytes.size() < 4)
        return false;
    const u32 tag = static_cast<u32>(bytes[0]) | (static_cast<u32>(bytes[1]) << 8) |
                    (static_cast<u32>(bytes[2]) << 16) | (static_cast<u32>(bytes[3]) << 24);
    return tag == ::whiteout::m2::MD20_TAG || tag == ::whiteout::m2::MD21_TAG;
}

// Group the M2 table's batches by the submesh they draw and publish the result
// as `surfaces` + a per-geoset range. Ordering inside a group is materialLayer
// then file order, which is the order the client draws a submesh's layers in.
//
// Runs at spawn, before the geosets exist: the range lands on the staged geoset
// and rides the normal staged→GPU copy, so nothing has to re-find it later.
void BuildM2Surfaces(Actor& actor) {
    const auto* table =
        static_cast<const profiles::wow::M2SurfaceTable*>(actor.render.surfaceTable.get());
    if (!table)
        return;

    const auto& src = table->Surfaces();
    std::vector<u32> order(src.size());
    for (u32 i = 0; i < order.size(); ++i)
        order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](u32 a, u32 b) {
        if (src[a].skinSectionIndex != src[b].skinSectionIndex)
            return src[a].skinSectionIndex < src[b].skinSectionIndex;
        return src[a].materialLayer < src[b].materialLayer;
    });

    auto& surfaces = actor.render.surfaces;
    surfaces.clear();
    surfaces.reserve(order.size());

    for (u32 pos = 0; pos < order.size();) {
        const u16 section = src[order[pos]].skinSectionIndex;
        const u32 begin = static_cast<u32>(surfaces.size());
        u32 count = 0;
        for (; pos < order.size() && src[order[pos]].skinSectionIndex == section; ++pos, ++count) {
            const auto& s = src[order[pos]];
            core::SurfaceKey key;
            key.model = core::ShadingModelId::M2Combiners;
            key.surface = order[pos];
            using profiles::wow::M2Blend;
            key.blend = (s.blend == M2Blend::Opaque)     ? core::BlendClass::Opaque
                        : (s.blend == M2Blend::AlphaKey) ? core::BlendClass::AlphaKey
                                                         : core::BlendClass::Transparent;
            key.priorityPlane = s.priorityPlane;
            key.sortOrder = static_cast<i16>(s.materialLayer);
            surfaces.push_back(key);
        }
        auto it = actor.render.stagedGeosets.find(static_cast<i32>(section));
        if (it != actor.render.stagedGeosets.end()) {
            it->second.surfaceBegin = begin;
            it->second.surfaceCount = count;
        }
    }
}
} // namespace
#endif

#if WDX_ENABLE_M3
namespace {
// "43DM" / "33DM" — MD34 (release) and MD33 (beta) written little-endian, so
// the four leading bytes read reversed. Spelled as the on-disk bytes rather
// than reusing the parser's tag constants, which live in WhiteoutLib's private
// `src/` tree and are stored pre-reversed for chunk comparison.
bool LooksLikeM3(std::span<const u8> bytes) {
    if (bytes.size() < 4)
        return false;
    return bytes[2] == 'D' && bytes[3] == 'M' && bytes[1] == '3' &&
           (bytes[0] == '4' || bytes[0] == '3');
}

// One surface per geoset — M3SurfaceTable entry g IS geoset g's resolved
// material, so unlike BuildM2Surfaces there is no grouping to do. Runs at
// spawn, before the geosets exist, for the same reason: the range lands on
// the staged geoset and rides the staged→GPU copy.
void BuildM3Surfaces(Actor& actor) {
    const auto* table =
        static_cast<const profiles::sc2_heroes::M3SurfaceTable*>(actor.render.surfaceTable.get());
    if (!table)
        return;
    const auto& src = table->Surfaces();
    auto& surfaces = actor.render.surfaces;
    surfaces.clear();
    surfaces.reserve(src.size());
    for (u32 g = 0; g < src.size(); ++g) {
        core::SurfaceKey key;
        key.model = core::ShadingModelId::M3Standard;
        key.surface = g;
        key.blend = profiles::sc2_heroes::M3ClassifySurface(src[g]).blend;
        key.priorityPlane = src[g].priority;
        const u32 begin = static_cast<u32>(surfaces.size());
        surfaces.push_back(key);
        auto it = actor.render.stagedGeosets.find(static_cast<i32>(g));
        if (it != actor.render.stagedGeosets.end()) {
            it->second.surfaceBegin = begin;
            it->second.surfaceCount = 1;
        }
    }
}
} // namespace
#endif

#if WDX_ENABLE_D3
namespace {
// One surface per emitted SubObject — D3SurfaceTable entry g IS geoset g's
// resolved material, so like BuildM3Surfaces there is no grouping to do.
void BuildD3Surfaces(Actor& actor) {
    const auto* table =
        static_cast<const profiles::diablo3::D3SurfaceTable*>(actor.render.surfaceTable.get());
    if (!table)
        return;
    const auto& src = table->Surfaces();
    auto& surfaces = actor.render.surfaces;
    surfaces.clear();
    surfaces.reserve(src.size());
    for (u32 g = 0; g < src.size(); ++g) {
        core::SurfaceKey key;
        key.model = core::ShadingModelId::D3Standard;
        key.surface = g;
        key.blend = profiles::diablo3::D3ClassifySurface(src[g]).blend;
        const u32 begin = static_cast<u32>(surfaces.size());
        surfaces.push_back(key);
        auto it = actor.render.stagedGeosets.find(static_cast<i32>(g));
        if (it != actor.render.stagedGeosets.end()) {
            it->second.surfaceBegin = begin;
            it->second.surfaceCount = 1;
        }
    }
}
} // namespace
#endif

Actor* ModelLoader::SpawnUnit(const ContentRef& ref, const Matrix44f& initialTm) {
    // Format detection by content, not by name. A fileDataID has no extension
    // to branch on, and that is exactly the reference a chunked `.m2` names
    // its siblings with — so sniffing the magic is the only rule that works
    // for both halves of ContentRef.
    if (auto* foreign = TrySpawnForeign(ref, initialTm))
        return foreign;

    if (!ref.IsPath())
        return nullptr; // not an M2/M3, and nothing else loads by id yet
    const u32 h = AddModelByPath(ref.path, initialTm);
    if (h == 0)
        return nullptr;
    return rs_.Scene().Actors().Find(h);
}

// Reads the bytes once, sniffs the magic, and routes an `.m2` or `.m3` through
// SpawnUnitFromSource. Returns null for anything else, including every MDX, so
// the caller falls through to the path route unchanged.
//
// Each format is compiled out with its own CMake option — see there for why
// they are opt-in rather than always present. With both off this reduces to
// `return nullptr` and the single read below disappears with it.
Actor* ModelLoader::TrySpawnForeign(const ContentRef& ref, const Matrix44f& initialTm) {
    auto* provider = rs_.Scene().ActiveContentProvider();
    if (!provider)
        return nullptr;
    auto bytes = provider->ReadFile(ref);
    if (!bytes || bytes->empty())
        return nullptr;
    const std::span<const ::whiteout::u8> data(bytes->data(), bytes->size());

    // WEM first, and unconditionally: the module is in whiteout_lib whatever
    // this build enables, and a `.wem` is not one of the three formats below —
    // it is a container that becomes one. Which one is the profile's answer,
    // and with nobody here to ask, the document's own default stands.
    if (auto document = io::ParseWemDocument(data, ref.Describe()))
        return SpawnWemDocument(*document, wem::ProfileId::Count, initialTm);

    // glTF next, and through the same machinery: the import is a
    // Generic-profile document that derives into Reforged on open
    // (GLTF_DESIGN §2). A `.glb` announces itself by magic; a bare-JSON
    // `.gltf` has none, so the ref's own extension is the fallback — the one
    // entry in this cascade that needs one.
    bool gltfByName = false;
    if (ref.IsPath()) {
        std::string lower = ref.path;
        for (char& c : lower)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        gltfByName = lower.ends_with(".gltf") || lower.ends_with(".glb");
    }
    if (io::LooksLikeGlb(data) || gltfByName) {
        if (auto document = io::ParseGltfDocument(data, ref.Describe()))
            return SpawnWemDocument(*document, wem::ProfileId::Count, initialTm);
        return nullptr; // It said glTF and did not parse; the log already says why.
    }

#if WDX_ENABLE_M2 || WDX_ENABLE_M3 || WDX_ENABLE_D3

    // Detection settles the scene's product. A model of a given format having
    // been recognised is direct evidence of one, and it outranks whatever the
    // product happened to be — including a value restored from the settings
    // file, which is where it usually comes from.
    //
    // This used to defer to any non-Neutral product on the grounds that an
    // explicit host choice should be left alone. In practice that made an
    // `.m2` opened in a scene left on `wc3` load its geometry and then silently
    // lose every texture: the product is what selects the storage the content
    // provider opens, so a WoW model in a WC3 scene has no WoW CASC to resolve
    // its fileDataID textures against, and each one falls back to white. The
    // profile is wrong for it too — WC3's WorldScale is 1 where WoW's is 100.
    // One scene renders one product; mixing them is what multi-scene is for.
    //
    // Decided from the magic alone, and *before* anything is parsed. Parsing an
    // `.m2` reads its `.skin` and `.anim` siblings back through this same
    // provider, so settling the product afterwards sent every one of those
    // reads to the previous game's storage — a guaranteed miss, and an install
    // opened to serve it.
#if WDX_ENABLE_M2
    const bool isM2 = LooksLikeM2(data);
#else
    constexpr bool isM2 = false;
#endif
#if WDX_ENABLE_M3
    const bool isM3 = !isM2 && LooksLikeM3(data);
#else
    constexpr bool isM3 = false;
#endif
#if WDX_ENABLE_D3
    // Every D3 SNO asset opens 0xDEADBEEF, so the magic identifies the family
    // and not the format; which group it is comes out of the version word (or
    // the ref's extension) once the product is settled. Last of the three
    // because it is the broadest test.
    const bool isD3 = !isM2 && !isM3 && io::LooksLikeD3(data);
#else
    constexpr bool isD3 = false;
#endif
    const ProductId product = isM2   ? ProductId::Wow
                              : isM3 ? ProductId::Sc2
                              : isD3 ? ProductId::D3
                                     : ProductId::Neutral;
    if (product == ProductId::Neutral)
        return nullptr;

    if (rs_.Scene().Product() != product) {
        const ProductId was = rs_.Scene().Product();
        rs_.Scene().SetProduct(product);
        // Detection changed the profile, so the host's re-stage trigger has to
        // fire exactly as it would for an explicit SceneView::SetProduct.
        rs_.Settings().MarkRenderModeDirty();
        if (was != ProductId::Neutral) {
            // Worth saying out loud: it also re-points the content provider at
            // a different game's storage, which is the difference between the
            // model's textures resolving and not.
            std::fprintf(stderr, "[model] '%s' is %s content; switching the scene from %s\n",
                         ref.Describe().c_str(), ProductName(product), ProductName(was));
        }
    }

    // Parse only now — the provider is pointed at the right game's storage.
    std::shared_ptr<IModelSource> source;
#if WDX_ENABLE_M2
    std::shared_ptr<io::M2ModelAdapter> m2;
    std::vector<profiles::wow::WowCharacterAppearance::SkinnedModel> skinnedModels;
    if (isM2) {
        m2 = io::M2ModelAdapter::Load(ref, data, provider, rs_.Settings().M2LazyAnimations());
        if (m2) {
            // Before Build(), which is where GetTextures turns the slots into
            // asset keys. The scene's product was settled above, so the tables
            // are read from the same install the model came from.
            auto& replaceables = WowReplaceables();
            replaceables.SetContentProvider(provider);
            replaceables.Apply(*m2, ref);

            // Characters and creatures fill disjoint slot families — 1/6/8
            // against 11/12/13 — so both run and at most one finds anything.
            auto& characters = WowCharacters();
            characters.SetContentProvider(provider);
            characters.Apply(*m2, ref, &skinnedModels);
        }
        source = m2;
    }
#endif
#if WDX_ENABLE_M3
    std::shared_ptr<io::M3ModelAdapter> m3;
    if (isM3) {
        // No provider needed to PARSE it: an `.m3` is one self-contained file
        // with no siblings to resolve, which is the whole difference from `.m2`.
        m3 = io::M3ModelAdapter::Load(ref, data);
        if (m3) {
            // Its animations are another matter. A hero's `.m3` carries almost
            // none of its own — Stand, Walk, Attack, the facial and portrait
            // sets are separate `.m3a` files, and nothing in the model names
            // them. The game reads them off the model's catalog entry, so we
            // do too; 1,331 Heroes models and 333 StarCraft II ones need it,
            // and without it they open in bind pose.
            //
            // Before the actor is built, like the `.m2` pair above: the merged
            // sequences have to be in GetSequences() by the time the animation
            // driver binds.
            auto& catalog = Sc2Catalog();
            catalog.SetContentProvider(provider);
            catalog.Apply(*m3, ref);
        }
        source = m3;
    }
#endif
#if WDX_ENABLE_D3
    std::shared_ptr<io::D3ModelAdapter> d3;
    if (isD3) {
        auto& cache = D3Cache();
        cache.SetContentProvider(provider);
        // An `.acr` names an `.app` and an AnimSet; an `.app` is the model
        // itself. Which one this is comes from the ref's extension where there
        // is one and from the version word otherwise — never from the magic,
        // which only says "some D3 asset".
        const auto group = io::D3GroupFor(ref, data);
        if (group == io::d3n::Group::Actor) {
            // The drawable is keyed on `(appearanceSno, lookIndex)` and not on
            // the `.acr`: 594 actors name one appearance, and keying on the
            // file that was asked for would build 594 separate drawables off
            // one correctly-shared parse. Neither the `.acr` nor the `.app` is
            // re-read to get here — both come out of D3SnoCache.
            d3 = D3Drawable(
                io::D3ModelAdapter::LoadActor(ref, data, cache, rs_.Settings().D3LazyAnimations()));
        } else if (group == io::d3n::Group::Appearance) {
            d3 = D3Drawable(io::D3ModelAdapter::LoadAppearance(ref, data, cache));
        } else {
            std::fprintf(stderr,
                         "[d3] '%s' is a Diablo III asset this build does not draw "
                         "(neither Actor nor Appearance)\n",
                         ref.Describe().c_str());
        }
        if (d3) {
            // Before the surface table, which resolves each geoset's material
            // at the look this chooses for it — and before GetTextures, which
            // collects exactly those variants' textures. A model with no
            // wardrobe is left alone and keeps drawing everything.
            D3Characters().Apply(*d3);
        }
        source = d3;
    }
#endif
    if (!source)
        return nullptr;

    Actor* actor = SpawnUnitFromSource(source, initialTm);
    if (!actor)
        return nullptr;

    FinishNativeActor(*actor, source);

#if WDX_ENABLE_M2
    // Not in FinishNativeActor: the collections models are what the character
    // *pass* asked for, and that pass runs off the ContentRef this route has
    // and the WEM route does not. A `.wem` names its own textures, so it has
    // nothing to look up.
    if (m2)
        SpawnWowSkinnedModels(*actor, *m2, skinnedModels, provider);
#endif
    return actor;
#else
    // Nothing but WEM is compiled in, and the arm above already answered.
    return nullptr;
#endif
}

// ---------------------------------------------------------------------------
// The per-format half of a spawn, shared by the file route and the WEM route
// ---------------------------------------------------------------------------

void ModelLoader::FinishNativeActor(Actor& actor, const std::shared_ptr<IModelSource>& source) {
    // Warcraft III keeps the scene's own shading model and gets its table from
    // Wc3TableFor on first use, so there is nothing to stamp — and stamping
    // Unlit would take it off the WC3 path entirely.
    if (dynamic_cast<io::MdxModelAdapter*>(source.get()))
        return;

    // Per actor, which model can draw this. Saying so per actor is what lets a
    // foreign model and a WC3 model coexist in one scene.
    actor.shadingModel = core::ShadingModelId::Unlit;

#if WDX_ENABLE_M2
    if (auto* m2 = dynamic_cast<io::M2ModelAdapter*>(source.get())) {
        // Built here rather than through GetMaterials: M2's per-batch binding
        // does not fit MaterialData, and the parsed model is right here.
        actor.render.surfaceTable =
            profiles::wow::BuildM2SurfaceTable(m2->SourceModel(), m2->ProfileIndex());
        BuildM2Surfaces(actor);
        actor.shadingModel = core::ShadingModelId::M2Combiners;
        return;
    }
#endif
#if WDX_ENABLE_D3
    if (auto d3 = std::dynamic_pointer_cast<io::D3ModelAdapter>(source)) {
        SetupD3Actor(actor, d3);
        return;
    }
#endif
#if WDX_ENABLE_M3
    if (auto* m3 = dynamic_cast<io::M3ModelAdapter*>(source.get())) {
        // Built off the raw model, M2's precedent — per-batch binding does not
        // fit MaterialData. Stamped only when something resolved: a table with
        // no valid entry (every material displacement / volume / …) leaves the
        // whole actor on Unlit, which draws where this model would vanish.
        auto table = profiles::sc2_heroes::BuildM3SurfaceTable(
            m3->SourceModel(), m3->EmittedRegions(), m3->EmittedMaterials());
        bool anyValid = false;
        for (const auto& s : table->Surfaces())
            anyValid |= s.valid;
        if (anyValid) {
            actor.render.surfaceTable = std::move(table);
            BuildM3Surfaces(actor);
            actor.shadingModel = core::ShadingModelId::M3Standard;
        }
        return;
    }
#endif
}

// ---------------------------------------------------------------------------
// WEM
// ---------------------------------------------------------------------------

Actor* ModelLoader::SpawnWem(const ContentRef& ref, wem::ProfileId profile,
                             const Matrix44f& initialTm) {
    auto* provider = rs_.Scene().ActiveContentProvider();
    if (!provider)
        return nullptr;
    auto bytes = provider->ReadFile(ref);
    if (!bytes || bytes->empty())
        return nullptr;
    auto document = io::ParseWemDocument(
        std::span<const ::whiteout::u8>(bytes->data(), bytes->size()), ref.Describe());
    if (!document) {
        std::fprintf(stderr, "[wem] '%s' is not a WEM file this build can read\n",
                     ref.Describe().c_str());
        return nullptr;
    }
    return SpawnWemDocument(*document, profile, initialTm);
}

Actor* ModelLoader::SpawnWemDocument(const io::WemDocument& document, wem::ProfileId profile,
                                     const Matrix44f& initialTm) {
    auto* provider = rs_.Scene().ActiveContentProvider();

    // The product is settled from the PROFILE and before anything is staged,
    // for the reason the format sniff above settles it before parsing: the
    // product picks the storage every texture reference resolves against, and
    // a document that names its textures by fileDataID resolves them against a
    // World of Warcraft install or against nothing at all.
    //
    // Which means the profile has to be decided first, and by the same call the
    // host's dialog decided it with, so the two cannot disagree.
    const wem::ProfileId chosen =
        profile == wem::ProfileId::Count ? io::DefaultWemProfile(document.document) : profile;
    if (chosen != wem::ProfileId::Count) {
        const ProductId product = io::ProductForWemProfile(chosen);
        if (rs_.Scene().Product() != product) {
            const ProductId was = rs_.Scene().Product();
            rs_.Scene().SetProduct(product);
            rs_.Settings().MarkRenderModeDirty();
            if (was != ProductId::Neutral) {
                std::fprintf(stderr,
                             "[wem] '%s' opens as %s content; switching the scene from %s\n",
                             document.name.c_str(), ProductName(product), ProductName(was));
            }
        }
        // Re-read: SetProduct re-points the provider at another game's storage.
        provider = rs_.Scene().ActiveContentProvider();
        // The same commitment AddModelByPath makes, for the same reason: a
        // Warcraft III `.wem` is Warcraft III content, and the day/night rig
        // and the environment probes are realised by nothing but a spawn. A
        // scene that skips this draws on the fixed studio light, which is not
        // an error and is not the game's lighting either.
        if (product == ProductId::Wc3)
            rs_.EnsureWc3GameData();
    }

    const io::WemSourceResult built = io::BuildWemSource(document, chosen, {}, provider,
#if WDX_ENABLE_D3
                                                         &D3Cache()
#else
                                                         nullptr
#endif
    );
    if (!built.ok()) {
        // The diagnostics are the most useful thing there is on this path: a
        // derive that cost 400 rows and a converter that refused the profile
        // say very different things, and the one-line error cannot tell them
        // apart.
        std::fprintf(stderr, "[wem] %s\n%s", built.error.c_str(),
                     io::DescribeWemDiagnostics(built.diagnostics).c_str());
        return nullptr;
    }
    if (!built.diagnostics.empty()) {
        // Not an error path. A lossy conversion that succeeded and one that
        // failed are different states and both have something to say — the
        // clips no `toX` writes are reported here every single time.
        std::fprintf(stderr, "[wem] '%s' as %s: %zu diagnostic(s)\n%s", document.name.c_str(),
                     wem::Profile(built.profile).displayName, built.diagnostics.size(),
                     io::DescribeWemDiagnostics(built.diagnostics).c_str());
    }

    // The scene product is two decisions at once — which render profile draws,
    // and which install every asset resolves against — and this is the one path
    // where they legitimately disagree. The profile keeps the product it was
    // opened as; the provider goes back to the game the CONTENT came from, or a
    // Diablo III appearance opened as Warcraft III resolves none of its SNOs
    // and stages a white placeholder per texture slot.
    //
    // It outlives the spawn on purpose: `Acquire` only records the reference and
    // the host's needs pump does the read, so the install has to still be the
    // right one several frames later. The next model added resets it, which is
    // the same one-game-per-scene rule every other path already follows.
    if (built.assetProduct != ProductId::Neutral && built.assetProduct != rs_.Scene().Product()) {
        if (auto* files = dynamic_cast<io::FileContentProvider*>(provider))
            files->SetGame(built.assetProduct);
    }

    Actor* actor = SpawnUnitFromSource(built.source, initialTm);
    if (!actor)
        return nullptr;
    // Restamped, because the spawn read it off the ACTIVE profile and this is
    // the one actor whose geometry may not be in that profile's units — a
    // Diablo III `.wem` opened as Warcraft III is still authored at 17.
    actor->worldScale = built.worldScale;
    FinishNativeActor(*actor, built.source);
    return actor;
}

#if WDX_ENABLE_M2
// Guarded from the outside, unlike its neighbours below: the declaration is
// too, because the signature itself names M2-only types.
void ModelLoader::SpawnWowSkinnedModels(
    Actor& character, io::M2ModelAdapter& characterAdapter,
    const std::vector<profiles::wow::WowCharacterAppearance::SkinnedModel>& wanted,
    io::IContentProvider* provider) {
    // Whatever it was wearing goes first: a restyle changes which parts the
    // collections model shows, and there is no cheaper way to say so than to
    // rebuild it — unlike geosets of the character itself, these are a
    // different file whose very identity a choice can change. Collected before
    // destroying, because DestroyActor edits the list being walked.
    std::vector<u32> worn;
    for (u32 h : character.children)
        if (auto* c = rs_.Scene().Actors().Find(h); c && c->role == ActorRole::Skinned)
            worn.push_back(h);
    for (u32 h : worn)
        DestroyActor(h);

    if (!provider || wanted.empty())
        return;

    for (const auto& want : wanted) {
        const ContentRef ref = ContentRef::FromFileId(want.fileId);
        auto bytes = provider->ReadFile(ref);
        if (!bytes || bytes->empty()) {
            std::fprintf(stderr, "[wow] collections model %u not readable\n", want.fileId);
            continue;
        }
        auto m2 = io::M2ModelAdapter::Load(
            ref, std::span<const ::whiteout::u8>(bytes->data(), bytes->size()), provider,
            rs_.Settings().M2LazyAnimations());
        if (!m2)
            continue;

        // Only the parts this appearance asked for. A collections model carries
        // every horn, frill and band the race offers in one file, so the set is
        // small and the default — everything draws — would be all of them at
        // once.
        m2->SetVisibleGeosets(want.geosets);
        // Its blank slots are the character's composites: type 1 is the body
        // sheet, 9 the horn colour, 20 the jewelry. Nothing of its own to
        // resolve, which is why there is no replaceable pass here.
        m2->SetComposedTextures(characterAdapter.ComposedTextures());

        const auto pairing =
            profiles::wow::PairBonesByKeyBone(characterAdapter.SourceModel(), m2->SourceModel());
        Actor* child = SpawnChildFromSource(character, ActorRole::Skinned, m2);
        if (!child)
            continue;
        child->skinnedParentBone = pairing;
        child->shadingModel = core::ShadingModelId::M2Combiners;
        child->render.surfaceTable =
            profiles::wow::BuildM2SurfaceTable(m2->SourceModel(), m2->ProfileIndex());
        BuildM2Surfaces(*child);
    }
}
#endif

std::shared_ptr<io::M2ModelAdapter> ModelLoader::ResolveParticleModel(const std::string& key) {
#if WDX_ENABLE_M2
    if (key.empty())
        return nullptr;
    auto cached = particleModels_.find(key);
    if (cached != particleModels_.end())
        return cached->second;

    std::shared_ptr<io::M2ModelAdapter> built;
    if (auto* provider = rs_.Scene().ActiveContentProvider()) {
        // `#<id>` is ContentRef::Describe's form, and GPID is the only thing
        // that names a geometry model in shipped data — no record in the corpus
        // carries the inline filename the pre-Legion layout had.
        const ContentRef ref = (key[0] == '#') ? ContentRef::FromFileId(static_cast<u32>(
                                                     std::strtoul(key.c_str() + 1, nullptr, 10)))
                                               : ContentRef::FromPath(key);
        if (auto bytes = provider->ReadFile(ref); bytes && !bytes->empty()) {
            built = io::M2ModelAdapter::Load(
                ref, std::span<const ::whiteout::u8>(bytes->data(), bytes->size()), provider,
                rs_.Settings().M2LazyAnimations());
        }
        if (!built)
            std::fprintf(stderr, "[wow] particle model %s not readable\n", key.c_str());
    }
    // Cached even when null: an emitter births every frame, and re-reading a
    // model that is not there would re-read it every frame.
    return particleModels_.emplace(key, std::move(built)).first->second;
#else
    (void)key;
    return nullptr;
#endif
}

void ModelLoader::PreloadModelParticleGeometry(u32 handle, const std::string& key) {
#if WDX_ENABLE_M2
    // Same job PreloadChildTemplates does for PE1, and for the same reason: the
    // first birth must not be the first time an asset is asked for. A PE1 child
    // is a template the AssetManager fetches, so holding its Model slot is
    // enough; a geometry model is parsed here, so what is left to warm is its
    // TEXTURES — and those are what a mid-capture need would otherwise be.
    auto model = ResolveParticleModel(key);
    auto* a = rs_.Scene().Actors().Find(handle);
    if (!model || !a)
        return;
    for (const auto& tex : model->GetTextures()) {
        if (tex.sharedKey.empty())
            continue;
        const ContentRef ref = (tex.sharedKey[0] == '#')
                                   ? ContentRef::FromFileId(static_cast<u32>(
                                         std::strtoul(tex.sharedKey.c_str() + 1, nullptr, 10)))
                                   : ContentRef::FromPath(tex.sharedKey);
        a->assetSlots.push_back(
            rs_.Assets().Acquire(AssetKind::Texture, assets::kSoleSubKind, ref));
    }
#else
    (void)handle;
    (void)key;
#endif
}

Actor* ModelLoader::SpawnModelParticle(Actor& owner, const std::string& key,
                                       const Matrix44f& initialTm, u32 forceHandle) {
#if WDX_ENABLE_M2
    auto model = ResolveParticleModel(key);
    if (!model)
        return nullptr;

    Actor* child = SpawnChildFromSource(owner, ActorRole::PE1, model, forceHandle);
    if (!child)
        return nullptr;
    child->worldTransform = initialTm;
    // The two things TrySpawnForeign sets by hand for a top-level `.m2`, and
    // that nothing on the child-spawn path does: without them the model draws
    // through the WC3 program with no surface bindings, which is a silhouette.
    child->shadingModel = core::ShadingModelId::M2Combiners;
    child->render.surfaceTable =
        profiles::wow::BuildM2SurfaceTable(model->SourceModel(), model->ProfileIndex());
    BuildM2Surfaces(*child);
    return child;
#else
    (void)owner;
    (void)key;
    (void)initialTm;
    (void)forceHandle;
    return nullptr;
#endif
}

Actor* ModelLoader::SpawnUnitFromSource(std::shared_ptr<IModelSource> source,
                                        const Matrix44f& initialTm, u32 forceHandle) {
    if (!source)
        return nullptr;

    ModelData data = source->Build();
    const u32 h =
        AddModel(data.meshes, data.textures, data.materials, data.skeleton, data.skinWeights,
                 data.pe2Configs, data.ribbonConfigs, data.collisionConfigs, forceHandle);
    Actor* actor = rs_.Scene().Actors().Find(h);
    if (!actor)
        return nullptr;

    actor->worldTransform = initialTm;
    actor->animation.Bind(source);
    // The template path stamps these from ModelTemplate; this one has no
    // template, so it takes them from the same Build() snapshot.
    actor->bounds = data.bounds;
    // Only `.m3` and `.app` carry cloth, and both spawn here — the template
    // path never sees a source with a soft-body solver behind it.
    for (auto& c : data.clothOverlays) {
        ClothOverlay overlay;
        overlay.def = std::move(c);
        actor->render.cloths.push_back(std::move(overlay));
    }

    if (!data.attachmentConfigs.empty())
        SetAttachmentConfigs(h, data.attachmentConfigs);
    if (!data.pe1Configs.empty())
        SetPE1Configs(h, data.pe1Configs);
    if (!data.m2ParticleConfigs.empty())
        SetM2ParticleConfigs(h, data.m2ParticleConfigs);

    // CornEffect (PopcornFX) emitters — mirror StageActor's template path so a
    // live source (e.g. a directly-loaded .pkb) gets its effect too. Per-frame
    // multipliers/visibility flow through FrameState::cornStates afterward.
    {
        const Vector4f teamRGBA = {
            ((actor->teamColor) & 0xFF) / 255.0f,
            ((actor->teamColor >> 8) & 0xFF) / 255.0f,
            ((actor->teamColor >> 16) & 0xFF) / 255.0f,
            1.0f,
        };
        for (const auto& cinit : data.cornEmitterInits) {
            if (cinit.pkbPath.empty())
                continue;
            auto em = std::make_unique<corn_effects::CornEffectsEmitter>(
                rs_.Assets(), cinit.pkbPath, cinit.animVisibilityGuide, cinit.replaceableId,
                cinit.cornEffectsScaling);
            em->SetEmissionRateMultiplier(cinit.defaultEmissionRate);
            em->SetLifeSpanMultiplier(cinit.defaultLifeSpan);
            em->SetSpeedMultiplier(cinit.defaultSpeed);
            em->SetColor(cinit.defaultColor);
            em->SetReplaceableColor(teamRGBA);
            rs_.CornEffects().AddCornEmitter(actor->handle, cinit.emitterId, std::move(em));
        }
    }

    if (!data.cornEmitterInits.empty()) {
        const Vector4f teamRGBA = {
            ((actor->teamColor) & 0xFF) / 255.0f,
            ((actor->teamColor >> 8) & 0xFF) / 255.0f,
            ((actor->teamColor >> 16) & 0xFF) / 255.0f,
            1.0f,
        };
        for (const auto& cinit : data.cornEmitterInits) {
            if (cinit.pkbPath.empty())
                continue;
            auto em = std::make_unique<corn_effects::CornEffectsEmitter>(
                rs_.Assets(), cinit.pkbPath, cinit.animVisibilityGuide, cinit.replaceableId,
                cinit.cornEffectsScaling);
            em->SetEmissionRateMultiplier(cinit.defaultEmissionRate);
            em->SetLifeSpanMultiplier(cinit.defaultLifeSpan);
            em->SetSpeedMultiplier(cinit.defaultSpeed);
            em->SetColor(cinit.defaultColor);
            em->SetReplaceableColor(teamRGBA);
            rs_.CornEffects().AddCornEmitter(actor->handle, cinit.emitterId, std::move(em));
        }
    }

    // Same as the template-based path: Acquire+hold slots so the host
    // pump fetches + parses each child MDX up front and refs are
    // released when the actor dies.
    PreloadChildTemplates(*actor, data.pe1Configs, data.attachmentConfigs);

    return actor;
}

void ModelLoader::UploadStagedTextures(Actor& mi) {
    if (!mi.render.textures)
        mi.render.textures = rs_.Textures().CreateModelScope(&rs_.Assets());
    for (auto& [id, st] : mi.render.stagedTextures) {
        // File-backed textures: bind through an AssetManager slot. The
        // slot exists from the moment we Acquire — placeholder white
        // until the host fetches and Apply pushes the real bytes; the
        // ModelScope picks up the swap automatically via Get().
        if (!st.sharedKey.empty()) {
            // `#<id>` is ContentRef::Describe's form for an id-addressed ref,
            // which is how a chunked `.m2` names its textures. Everything else
            // is a path.
            const ContentRef ref = (st.sharedKey[0] == '#')
                                       ? ContentRef::FromFileId(static_cast<u32>(
                                             std::strtoul(st.sharedKey.c_str() + 1, nullptr, 10)))
                                       : ContentRef::FromPath(st.sharedKey);
            // Cube outranks linear: the only cube slot is the environment map,
            // which is colour, so the two never contend.
            const assets::AssetSubKind subKind = st.cubeMap      ? assets::kTextureCubeSubKind
                                                 : st.linearData ? assets::kTextureLinearSubKind
                                                                 : assets::kSoleSubKind;
            const auto slot = rs_.Assets().Acquire(AssetKind::Texture, subKind, ref);
            mi.render.textures->BindSlot(id, slot, st.wrapFlags);
            continue;
        }
        // Synthetic (no path, just CPU-side pixels — e.g. the 4x4 white
        // the MDX adapter generates for textures with no file name).
        if (st.width <= 0 || st.height <= 0)
            continue;
        const gfx::Format texFormat =
            (st.format == gfx::Format::Unknown) ? gfx::Format::R8G8B8A8_UNORM : st.format;
        const gfx::TextureDesc desc{
            .width = st.width,
            .height = st.height,
            .mipLevels = (std::max)(1, st.mipLevels),
            .format = texFormat,
            .usage = gfx::TextureUsage::ShaderResource,
        };
        mi.render.textures->Upload(id, desc, st.pixels.data(), st.wrapFlags);
    }
    mi.render.stagedTextures.clear();
}

void ModelLoader::uploadTemplateGpu(ModelTemplate& tmpl) {
    if (tmpl.gpuUploaded)
        return;
    // Stash the device so ~ModelTemplate can auto-release the shared
    // GPU buffers when the last actor's sourceTemplate strong-ref
    // drops (templates live in the manager's weak cache now, so
    // there's no other strong owner to call ReleaseGPU explicitly).
    tmpl.gpuDevice = rs_.Pipeline().Gfx();
    tmpl.sharedGeosets.clear();
    tmpl.sharedGeosets.reserve(tmpl.meshes.size());

    std::unordered_map<i32, const SkinWeightData*> weightsByGeoset;
    weightsByGeoset.reserve(tmpl.skinWeights.size());
    for (const auto& sw : tmpl.skinWeights)
        weightsByGeoset[sw.geosetId] = &sw;

    for (const auto& mesh : tmpl.meshes) {
        ModelTemplate::SharedGeoset sg;
        sg.geosetId = mesh.geosetId;
        sg.materialId = mesh.materialId;
        sg.lod = mesh.lod;
        sg.vertexCount = (i32)mesh.positions.size();
        sg.indexCount = (i32)mesh.indices.size();
        sg.localCentroid =
            GeosetBoundsCenter(sg.vertexCount, [&](i32 i) { return mesh.positions[i]; });

        std::vector<Vertex> vertices(sg.vertexCount);
        for (i32 i = 0; i < sg.vertexCount; i++) {
            vertices[i].position = mesh.positions[i];
            vertices[i].normal =
                (i < (i32)mesh.normals.size()) ? mesh.normals[i] : Vector3f{0, 0, 1};
            vertices[i].uv = (i < (i32)mesh.uvs.size()) ? mesh.uvs[i] : Vector2f{0, 0};
            vertices[i].color = {1.0f, 1.0f, 1.0f, 1.0f};
        }
        sg.unskinnedVb = rs_.Pipeline().Gfx()->CreateBuffer(
            {
                .size = (u32)(sizeof(Vertex) * sg.vertexCount),
                .usage = gfx::BufferUsage::Vertex,
            },
            vertices.data());

        const bool hasUv1Data = (i32)mesh.uvs1.size() == sg.vertexCount && sg.vertexCount > 0;
        bool wantsUv1 = false;
        if (hasUv1Data && mesh.materialId >= 0) {
            for (const auto& mat : tmpl.materials) {
                if (mat.materialId != mesh.materialId)
                    continue;
                for (const auto& lay : mat.layers) {
                    if (lay.coordId == 1) {
                        wantsUv1 = true;
                        break;
                    }
                }
                break;
            }
        }
        if (wantsUv1) {
            std::vector<Vertex> verticesUv1(sg.vertexCount);
            for (i32 i = 0; i < sg.vertexCount; i++) {
                verticesUv1[i].position = mesh.positions[i];
                verticesUv1[i].normal =
                    (i < (i32)mesh.normals.size()) ? mesh.normals[i] : Vector3f{0, 0, 1};
                verticesUv1[i].uv = mesh.uvs1[i];
                verticesUv1[i].color = {1.0f, 1.0f, 1.0f, 1.0f};
            }
            sg.unskinnedVb1 = rs_.Pipeline().Gfx()->CreateBuffer(
                {
                    .size = (u32)(sizeof(Vertex) * sg.vertexCount),
                    .usage = gfx::BufferUsage::Vertex,
                },
                verticesUv1.data());
        }

        sg.ib = rs_.Pipeline().Gfx()->CreateBuffer(
            {
                .size = (u32)(sizeof(u32) * sg.indexCount),
                .usage = gfx::BufferUsage::Index,
            },
            mesh.indices.data());

        if ((i32)mesh.tangents.size() == sg.vertexCount) {
            sg.tangentVb = rs_.Pipeline().Gfx()->CreateBuffer(
                {
                    .size = (u32)(sizeof(Vector4f) * sg.vertexCount),
                    .usage = gfx::BufferUsage::Vertex,
                },
                mesh.tangents.data());
        }

        auto wIt = weightsByGeoset.find(mesh.geosetId);
        if (wIt != weightsByGeoset.end() && (i32)wIt->second->influences.size() == sg.vertexCount) {
            const auto& sw = *wIt->second;
            std::vector<BoneVertex> bv(sg.vertexCount);
            for (i32 v = 0; v < sg.vertexCount; v++) {
                const auto& inf = sw.influences[v];
                i32 idxArr[4] = {inf.boneIdx[0], inf.boneIdx[1], inf.boneIdx[2], inf.boneIdx[3]};
                f32 wtArr[4] = {inf.weight[0], inf.weight[1], inf.weight[2], inf.weight[3]};
                bls::PackBoneVertex(bv[v], idxArr, wtArr);
            }
            sg.boneVb = rs_.Pipeline().Gfx()->CreateBuffer(
                {
                    .size = (u32)(sizeof(BoneVertex) * sg.vertexCount),
                    .usage = gfx::BufferUsage::Vertex,
                },
                bv.data());
        }

        tmpl.sharedGeosets.push_back(sg);
    }

    // Template-side textures used to be pre-bound here against the
    // legacy shared cache. With AssetManager, texture lifetimes are
    // entirely actor-owned (Actor.render.textures takes the slot ref).
    // Pre-binding on the template would just double-refcount the slot
    // for no benefit, so the loop is gone.
    tmpl.gpuUploaded = true;
}

void ModelLoader::UploadStagedGeosets(Actor& mi) {
    if (mi.sourceTemplate) {

        auto& tmpl = *mi.sourceTemplate;
        uploadTemplateGpu(tmpl);
        if (mi.render.gpuGeosets.empty()) {

            for (const auto& shared : tmpl.sharedGeosets) {
                GPUGeoset gg;
                gg.geosetId = shared.geosetId;
                gg.materialId = shared.materialId;
                gg.lod = shared.lod;
                gg.ib = shared.ib;
                gg.unskinnedVb = shared.unskinnedVb;
                gg.unskinnedVb1 = shared.unskinnedVb1;
                gg.tangentVb = shared.tangentVb;
                gg.boneVb = shared.boneVb;
                gg.indexCount = shared.indexCount;
                gg.vertexCount = shared.vertexCount;
                gg.localCentroid = shared.localCentroid;
                gg.hasSkinning = true;
                if (auto* t = Wc3TableFor(mi.render))
                    if (const auto* m = t->Material(shared.materialId))
                        gg.priorityPlane = m->cpu.priorityPlane;
                mi.render.gpuGeosets.push_back(gg);
            }
        } else {

            for (auto& gg : mi.render.gpuGeosets) {
                if (auto* t = Wc3TableFor(mi.render))
                    if (const auto* m = t->Material(gg.materialId))
                        gg.priorityPlane = m->cpu.priorityPlane;
            }
        }
        mi.render.stagedGeosets.clear();
    } else {

        for (auto& [id, sg] : mi.render.stagedGeosets) {
            GPUGeoset gg;
            gg.geosetId = id;
            gg.materialId = sg.materialId;
            gg.lod = sg.lod;
            const bool baked = sg.baked.Valid();
            gg.indexCount = (i32)sg.indices.size();
            gg.vertexCount = baked ? sg.bakedVertexCount : (i32)sg.vertices.size();
            gg.localCentroid = sg.centroid;
            gg.surfaceBegin = sg.surfaceBegin;
            gg.surfaceCount = sg.surfaceCount;
            gg.hasSkinning = true;

            if (auto* t = Wc3TableFor(mi.render))
                if (const auto* m = t->Material(sg.materialId))
                    gg.priorityPlane = m->cpu.priorityPlane;

            const GeosetSkinInfo* skinInfo = mi.render.skinning.GetGeosetWeights(id);

            // Two shapes, one buffer. A baked blob goes up byte for byte and
            // brings its own stride and layout description; everything else
            // is WC3's interleaved Vertex at layout 0, exactly as before.
            if (baked) {
                gg.baseStride = sg.baked.stride;
                gg.layoutId = rs_.Pipeline().VertexLayouts().Intern(sg.baked.attributes);
                gg.unskinnedVb = rs_.Pipeline().Gfx()->CreateBuffer(
                    {
                        .size = (u32)sg.baked.data.size(),
                        .usage = gfx::BufferUsage::Vertex,
                    },
                    sg.baked.data.data());
                // The one place the upload bytes are still in hand. A deformed
                // geoset rewrites two of its attributes per frame and needs the
                // other four from somewhere, and reading them back off the GPU
                // is not somewhere.
                if (sg.deformable)
                    mi.render.deformStaging[id] = sg.baked.data;
            } else {
                gg.unskinnedVb = rs_.Pipeline().Gfx()->CreateBuffer(
                    {
                        .size = (u32)(sizeof(Vertex) * sg.vertices.size()),
                        .usage = gfx::BufferUsage::Vertex,
                    },
                    sg.vertices.data());
            }

            gg.ib = rs_.Pipeline().Gfx()->CreateBuffer(
                {
                    .size = (u32)(sizeof(u32) * sg.indices.size()),
                    .usage = gfx::BufferUsage::Index,
                },
                sg.indices.data());

            if ((i32)sg.tangents.size() == gg.vertexCount) {
                gg.tangentVb = rs_.Pipeline().Gfx()->CreateBuffer(
                    {
                        .size = (u32)(sizeof(Vector4f) * sg.tangents.size()),
                        .usage = gfx::BufferUsage::Vertex,
                    },
                    sg.tangents.data());
            }

            if (skinInfo && (i32)skinInfo->vertices.size() == gg.vertexCount) {
                std::vector<BoneVertex> bv(gg.vertexCount);
                for (i32 v = 0; v < gg.vertexCount; v++) {
                    const auto& inf = skinInfo->vertices[v];
                    i32 idxArr[4] = {inf.boneIdx[0], inf.boneIdx[1], inf.boneIdx[2],
                                     inf.boneIdx[3]};
                    f32 wtArr[4] = {inf.weight[0], inf.weight[1], inf.weight[2], inf.weight[3]};
                    bls::PackBoneVertex(bv[v], idxArr, wtArr);
                }
                gg.boneVb = rs_.Pipeline().Gfx()->CreateBuffer(
                    {
                        .size = (u32)(sizeof(BoneVertex) * gg.vertexCount),
                        .usage = gfx::BufferUsage::Vertex,
                    },
                    bv.data());
            }

            mi.render.gpuGeosets.push_back(gg);
        }
        mi.render.stagedGeosets.clear();
    }

    mi.render.hasLods = false;
    for (const auto& g : mi.render.gpuGeosets) {
        if (g.lod != 0 && g.lod != 0xFFFFFFFFu) {
            mi.render.hasLods = true;
            break;
        }
    }
}

void ModelLoader::CreateNodePalette(Actor& mi) {
    auto& skinning = mi.render.skinning;

    // A geoset is skinned when there is something to read weights *from*.
    // Historically that was always the separate `BoneVertex` stream, so its
    // presence doubled as the test. That stops being true once a format ships
    // its weights inside the interleaved buffer — `.m3` does, and building a
    // second stream to carry the same numbers is exactly what its adapter
    // avoids — so the question is asked directly instead.
    const auto& layouts = rs_.Pipeline().VertexLayouts();
    auto skinnable = [&](const GPUGeoset& geo) {
        if (geo.boneVb != gfx::BufferHandle::Invalid)
            return true;
        return geo.layoutId != core::VertexLayoutCache::kWc3Interleaved &&
               layouts.Has(geo.layoutId, core::VertexSemantic::BoneWeights) &&
               layouts.Has(geo.layoutId, core::VertexSemantic::BoneIndices);
    };

    if (skinning.UsesPerActorPalette()) {
        // Path A: a single CB per actor, shared across every geoset.
        // The CB is sized to the full kMaxBones-slot shader struct so
        // descriptor-binding range checks (Vulkan) don't trip on a
        // smaller buffer — we just never write or read past the
        // actor's actorPaletteSize. The per-geoset slots stay Invalid
        // and `geo.hasSkinning` is still set so the draw path knows
        // skinning is active.
        if (skinning.ActorPaletteCb() == gfx::BufferHandle::Invalid &&
            skinning.ActorPaletteSize() > 0) {
            // Per-actor CB: mapped exactly once per frame per actor in
            // UpdateAnimation. The ring only needs enough slots to
            // cover the in-flight frames (one slot per concurrent
            // frame). Setting a small hint here is essential — every
            // actor instantiates its own CB, so scaling slots with the
            // global default would blow per-actor memory by hundreds of
            // megabytes on PE1-heavy scenes.
            //
            // Initial data is an identity palette so the very first
            // frame's draw — which happens before UpdateAnimation has
            // populated real matrices — reads bind-pose transforms
            // instead of zero matrices (which collapse every skinned
            // vertex to the origin).
            bls::BonePaletteCb identity{};
            for (i32 i = 0; i < bls::kMaxBones; ++i)
                bls::PackBone(identity.bones[i], Matrix44f::identity());
            gfx::BufferHandle cb = rs_.Pipeline().Gfx()->CreateBuffer(
                {
                    .size = sizeof(bls::BonePaletteCb),
                    .usage = gfx::BufferUsage::Constant | gfx::BufferUsage::CpuWritable,
                    .ringSlotsHint = 4,
                },
                &identity);
            skinning.SetActorPaletteCb(cb);
        }
        for (auto& geo : mi.render.gpuGeosets) {
            if (!skinnable(geo))
                continue;
            geo.hasSkinning = true;
            // geo.bonePaletteCb stays Invalid; draw path falls back to
            // skinning.ActorPaletteCb() — see render_pipeline.cpp.
        }
        return;
    }

    // Path B (fallback): one CB per geoset, unchanged from the
    // pre-Path-A behavior. Used when the actor's palette size would
    // exceed kActorPaletteCap (rare — typically very large WoW rigs).
    // Like the Path A per-actor CB, each per-geoset CB is mapped
    // exactly once per frame, so we only need kFramesInFlight slots.
    bls::BonePaletteCb identity{};
    for (i32 i = 0; i < bls::kMaxBones; ++i)
        bls::PackBone(identity.bones[i], Matrix44f::identity());
    for (auto& geo : mi.render.gpuGeosets) {
        if (!skinnable(geo))
            continue;
        if (geo.bonePaletteCb != gfx::BufferHandle::Invalid)
            continue;
        if (skinning.GeosetPaletteSize(geo.geosetId) <= 0)
            continue;
        geo.bonePaletteCb = rs_.Pipeline().Gfx()->CreateBuffer(
            {
                .size = sizeof(bls::BonePaletteCb),
                .usage = gfx::BufferUsage::Constant | gfx::BufferUsage::CpuWritable,
                .ringSlotsHint = 4,
            },
            &identity);
        geo.hasSkinning = true;
    }
}

void ModelLoader::CommitPendingUploads() {
    // Reap actors flagged by RequestClearAll(). Collect handles first to avoid mutating
    // the actor map while iterating.
    std::vector<u32> toReap;
    for (auto& [h, mi] : rs_.Scene().Actors().All())
        if (mi->render.stagedClear)
            toReap.push_back(h);
    for (u32 h : toReap)
        DestroyActor(h);

    for (auto& [h, miPtr] : rs_.Scene().Actors().All()) {
        auto* mi = miPtr.get();
        if (!mi->render.stagedDirty && !mi->render.skinDirty)
            continue;

        if (mi->render.stagedDirty) {
            UploadStagedTextures(*mi);

            // Rebuild the actor's table in place. UpdateMaterials (public
            // API, called by the Max plugin's RefreshMaterials) lands here too,
            // which is why its signature survives: MaterialData is unchanged,
            // only its owner moved.
            // Skipped for an actor whose table belongs to another product: its
            // surfaces were built at spawn and there are no MaterialData to
            // drain into a WC3 table that does not exist.
            if (auto* table = Wc3TableFor(mi->render)) {
                auto& mats = table->Materials();
                for (auto& [id, sm] : mi->render.stagedMaterials) {
                    if ((i32)mats.size() <= id)
                        mats.resize(id + 1);
                    mats[id].cpu = sm;
                }
                mi->render.surfaces.resize(mats.size());
            }
            mi->render.stagedMaterials.clear();

            UploadStagedGeosets(*mi);
            mi->render.stagedDirty = false;
        }

        if (mi->render.skinDirty) {
            CreateNodePalette(*mi);
            mi->render.skinDirty = false;
        }
    }
}

} // namespace whiteout::flakes::renderer::model
