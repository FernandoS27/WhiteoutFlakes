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
#include "model/model_instance.h"
#include "model/model_template.h"
#include "model/model_template_manager.h"
#include "particle/child_model_emitter.h"
#include "particle/particle_adapters.h"
#include "particle/particle2_emitter.h"
#include "render_service.h"
#include "render_service_impl.h"
#include "scene_manager.h"

#include "dbg_print.h"

#include "renderer/core/render_profile.h" // IRenderProfile::WorldScale
#if WDX_ENABLE_M3
#include "io/m3/m3_model_adapter.h"
#endif
#if WDX_ENABLE_M2
#include "io/m2/m2_model_adapter.h"
#include "renderer/profiles/wow/m2_surface_table.h"
#include <whiteout/models/m2/types.h>
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
using namespace ::whiteout::flakes::io;

namespace {
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
        if (slot != 0) rs_.Assets().Release(slot);
    }
    a.assetSlots.clear();
    rs_.Replaceables().UnregisterModel(a);
    if (rs_.Pipeline().Gfx())
        a.ReleaseGPU(*rs_.Pipeline().Gfx());
    actors.erase(it);
    rs_.Particles().RemoveModel(handle);
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
        em->SetSeed(particle::MixSeed(handle, 0x8000u + (u32)i));
        rs_.Particles().AddEmitter(handle, particle::ParticleOutput::ChildModel, i,
                                   std::move(em));
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
        if (ce.pkbPath.empty()) continue;
        if (!seenPkb.insert(ce.pkbPath).second) continue;
        a.assetSlots.push_back(
            rs_.Assets().Acquire(assets::AssetKind::Effect, assets::kSoleSubKind, ce.pkbPath));
    }
}

void ModelLoader::PreloadChildTemplates(Actor& a,
                                        const std::vector<PE1EmitterConfig>& pe1Cfgs,
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
        if (path.empty()) return;
        if (!seen.insert(path).second) return;
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
    for (i32 i = 0; i < (i32)tmpl->pe2Configs.size(); i++) {
        auto em = std::make_unique<particle::Emitter2>();
        em->SetDesc(tmpl->pe2Descs[i]);
        // Seed from stable identity, not construction order, so the same scene
        // reproduces its particle motion across runs.
        em->SetSeed(particle::MixSeed(mi->handle, (u32)i));
        rs_.Particles().AddEmitter(mi->handle, particle::ParticleOutput::Billboard, i,
                                   std::move(em));
    }
    mi->render.pe2State.resize(tmpl->pe2Configs.size());

    for (i32 i = 0; i < (i32)tmpl->ribbonConfigs.size(); i++)
        mi->render.ribbons.AddEmitter(i, tmpl->ribbonConfigs[i]);

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
            rs_.Assets(), cinit.pkbPath, cinit.animVisibilityGuide,
            cinit.replaceableId, cinit.cornEffectsScaling);
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

u32 ModelLoader::AddModel(const std::vector<MeshData>& meshes,
                          const std::vector<TextureData>& textures,
                          const std::vector<MaterialData>& materials, const SkeletonData& skeleton,
                          const std::vector<SkinWeightData>& skinWeights,
                          const std::vector<ParticleEmitterConfig>& particleConfigs,
                          const std::vector<RibbonEmitterConfig>& ribbonConfigs,
                          const std::vector<CollisionShapeData>& collisions) {
    u32 handle = rs_.Scene().AllocActorId();
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

    for (auto& tex : textures) {
        StagedTexture& st = mi->render.stagedTextures[tex.textureId];
        st.width = tex.width;
        st.height = tex.height;
        st.mipLevels = tex.mipLevels;
        st.replaceableId = tex.replaceableId;
        st.wrapFlags = tex.wrapFlags;
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

    for (auto& mesh : meshes) {
        StagedGeoset& sg = mi->render.stagedGeosets[mesh.geosetId];
        sg.materialId = mesh.materialId;
        sg.lod = mesh.lod;
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

    for (usize i = 0; i < particleConfigs.size(); i++) {
        const auto& pcfg = particleConfigs[i];
        auto em = std::make_unique<particle::Emitter2>();
        em->SetDesc(particle::DescFromWc3Config(pcfg));
        em->SetSeed(particle::MixSeed(handle, (u32)i));
        rs_.Particles().AddEmitter(handle, particle::ParticleOutput::Billboard, (i32)i,
                                   std::move(em));
    }
    mi->render.pe2State.resize(particleConfigs.size());

    for (usize i = 0; i < ribbonConfigs.size(); i++) {
        mi->render.ribbons.AddEmitter((i32)i, ribbonConfigs[i]);
    }

    for (auto& cs : collisions) {
        CollisionShape shape;
        shape.type = cs.type;
        shape.vmin = cs.vertices[0];
        shape.vmax = cs.vertices[1];
        shape.radius = cs.radius;
        shape.pivot = cs.pivot;
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
    const auto* table = static_cast<const profiles::wow::M2SurfaceTable*>(
        actor.render.surfaceTable.get());
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
#if WDX_ENABLE_M2 || WDX_ENABLE_M3
    auto* provider = rs_.Scene().ActiveContentProvider();
    if (!provider)
        return nullptr;
    auto bytes = provider->ReadFile(ref);
    if (!bytes || bytes->empty())
        return nullptr;
    const std::span<const ::whiteout::u8> data(bytes->data(), bytes->size());

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
    const ProductId product = isM2   ? ProductId::Wow
                              : isM3 ? ProductId::Sc2
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
    if (isM2) {
        m2 = io::M2ModelAdapter::Load(ref, data, provider);
        source = m2;
    }
#endif
#if WDX_ENABLE_M3
    if (isM3) {
        // No provider: `.m3` is one self-contained file with no siblings to
        // resolve, which is the whole difference from `.m2`.
        source = io::M3ModelAdapter::Load(ref, data);
    }
#endif
    if (!source)
        return nullptr;

    Actor* actor = SpawnUnitFromSource(std::move(source), initialTm);
    if (!actor)
        return nullptr;

    // Per actor, which model can draw this. Saying so per actor is what lets a
    // foreign model and a WC3 model coexist in one scene.
    actor->shadingModel = core::ShadingModelId::Unlit;

#if WDX_ENABLE_M2
    if (m2) {
        // Built here rather than through GetMaterials: M2's per-batch binding
        // does not fit MaterialData, and the parsed model is right here.
        actor->render.surfaceTable =
            profiles::wow::BuildM2SurfaceTable(m2->SourceModel(), m2->ProfileIndex());
        BuildM2Surfaces(*actor);
        actor->shadingModel = core::ShadingModelId::M2Combiners;
    }
#endif
    return actor;
#else
    (void)ref;
    (void)initialTm;
    return nullptr;
#endif
}

Actor* ModelLoader::SpawnUnitFromSource(std::shared_ptr<IModelSource> source,
                                        const Matrix44f& initialTm) {
    if (!source)
        return nullptr;

    ModelData data = source->Build();
    const u32 h =
        AddModel(data.meshes, data.textures, data.materials, data.skeleton, data.skinWeights,
                 data.pe2Configs, data.ribbonConfigs, data.collisionConfigs);
    Actor* actor = rs_.Scene().Actors().Find(h);
    if (!actor)
        return nullptr;

    actor->worldTransform = initialTm;
    actor->animation.Bind(source);
    // The template path stamps these from ModelTemplate; this one has no
    // template, so it takes them from the same Build() snapshot.
    actor->bounds = data.bounds;

    if (!data.attachmentConfigs.empty())
        SetAttachmentConfigs(h, data.attachmentConfigs);
    if (!data.pe1Configs.empty())
        SetPE1Configs(h, data.pe1Configs);

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
            const ContentRef ref =
                (st.sharedKey[0] == '#')
                    ? ContentRef::FromFileId(
                          static_cast<u32>(std::strtoul(st.sharedKey.c_str() + 1, nullptr, 10)))
                    : ContentRef::FromPath(st.sharedKey);
            const auto slot = rs_.Assets().Acquire(AssetKind::Texture, assets::kSoleSubKind, ref);
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
            if (geo.boneVb == gfx::BufferHandle::Invalid)
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
        if (geo.boneVb == gfx::BufferHandle::Invalid)
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
