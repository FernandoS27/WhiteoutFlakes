// ============================================================================
// ModelLoader — extracted from RenderService.
//
// Owns model creation, staging, and per-frame GPU-upload commit. Reaches into
// RenderService::Impl directly via friend access (same pattern used by
// FrameTicker and the other in-tree subsystems — see render_service_impl.h).
// ============================================================================

#include "model/model_loader.h"

#include "render_service.h"
#include "render_service_impl.h"
#include "model/model_instance.h"
#include "model/model_template.h"
#include "model/model_template_manager.h"
#include "../io/mdx_model_adapter.h"
#include "assets/replaceable_texture_manager.h"
#include "assets/texture_asset_manager.h"
#include "assets/sampler_asset_manager.h"
#include "scene_manager.h"
#include "effects/spn_spawner.h"
#include "particle/plane_emitter.h"
#include "bls/bls_cb_layout.h"
#include "bls/bls_draw_helpers.h"

#include "dbg_print.h"

#include <algorithm>
#include <cstring>
#include <unordered_map>

namespace whiteout::flakes::renderer::model {

using namespace ::whiteout::flakes::renderer::animation;
using namespace ::whiteout::flakes::renderer::effects;
using namespace ::whiteout::flakes::renderer::particle;
using namespace ::whiteout::flakes::renderer::assets;
using namespace ::whiteout::flakes::renderer::bls;
using namespace ::whiteout::flakes::io;

ModelLoader::ModelLoader(RenderService& rs) : rs_(rs) {}
ModelLoader::~ModelLoader() = default;

Actor* ModelLoader::SpawnChild(Actor& parent,
                               ActorRole role,
                               std::shared_ptr<ModelTemplate> tmpl,
                               const Matrix44f& initialTm,
                               u32 forceHandle) {
    if (!tmpl) return nullptr;

    const u32 childH = forceHandle != 0 ? forceHandle
                                        : rs_.Scene().AllocActorId();
    auto child = std::make_unique<Actor>();
    child->handle         = childH;
    child->parent         = parent.handle;
    child->role           = role;
    child->treeDepth      = parent.treeDepth + 1;
    child->worldTransform = initialTm;
    child->teamColor      = parent.teamColor;   // inherit
    if (tmpl->adapter) child->animation.Bind(tmpl->adapter);

    // Default birth-time policy by role: Attachment / PE1 children pause with
    // their ancestor (use ancestor's actorTimeMs so paused parents pause their
    // visual children too); SPN children continue using the wall clock since
    // their lifetime is wall-clock-managed by SpnSpawner.
    // Callers may override via animation.SetBirthTimeMs after this call.
    if (role == ActorRole::SPN) {
        child->animation.SetBirthTimeMs(rs_.Scene().GetAnimationTime());
    } else {
        child->animation.SetBirthTimeMs(
            AncestorActorTimeMs(parent, rs_.Scene().Actors()));
    }

    StageActor(child.get(), tmpl);
    parent.children.push_back(childH);

    Actor* ptr = child.get();
    rs_.Scene().Actors().All()[childH] = std::move(child);
    if (role == ActorRole::PE1) rs_.Scene().IncrementPE1Instances();
    return ptr;
}

void ModelLoader::DestroyActor(u32 handle) {
    auto& actors = rs_.Scene().Actors().All();
    auto it = actors.find(handle);
    if (it == actors.end()) return;

    // Recurse into children first — destroying them while the parent's
    // services are still alive avoids any reentrant lookups.
    // Copy because each DestroyActor call mutates the parent's vector.
    auto childList = it->second->children;
    for (u32 ch : childList) DestroyActor(ch);

    it = actors.find(handle);
    if (it == actors.end()) return;
    Actor& a = *it->second;

    if (a.parent != 0) {
        if (auto* p = rs_.Scene().Actors().Find(a.parent)) {
            auto& cs = p->children;
            cs.erase(std::remove(cs.begin(), cs.end(), handle), cs.end());
        }
    }

    if (a.role == ActorRole::PE1)
        rs_.Scene().DecrementPE1Instances();

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
    if (!mi) return;
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
    if (!mi) return;
    for (i32 i= 0; i < (i32)configs.size(); i++)
        mi->render.pe1.AddEmitter(i, configs[i]);
}


void ModelLoader::StageActor(Actor* mi,
                             std::shared_ptr<ModelTemplate> tmpl) {
    if (!tmpl) return;

    mi->sourceTemplate = tmpl;

    if (tmpl->adapter) mi->animation.Bind(tmpl->adapter);

    for (auto& tex : tmpl->textures) {
        StagedTexture& st = mi->render.stagedTextures[tex.textureId];
        st.width = tex.width; st.height = tex.height;
        st.mipLevels = tex.mipLevels;
        st.replaceableId = tex.replaceableId;
        st.wrapFlags = tex.wrapFlags;
        st.format = tex.format;
        st.sharedKey = tex.sharedKey;
        const bool alreadyCached =
            !tex.sharedKey.empty() && IsTextureCached(tex.sharedKey);
        if (!alreadyCached) st.pixels = tex.pixels;
        if (tex.replaceableId != 0)
            rs_.Replaceables().RegisterModelSlot(*mi, tex.textureId, tex.replaceableId);
    }

    for (auto& mat : tmpl->materials) {
        StagedMaterial& sm = mi->render.stagedMaterials[mat.materialId];
        sm.layers        = mat.layers;
        sm.priorityPlane = mat.priorityPlane;
        sm.sortOrder     = mat.sortOrder;
    }

    if (tmpl->skinningData && tmpl->skinningData->nodeCount > 0) {
        mi->render.skinning.SetSharedData(tmpl->skinningData);

        mi->render.skinDirty = true;
    }

    for (i32 i= 0; i < (i32)tmpl->pe2Configs.size(); i++) {
        const auto& pcfg = tmpl->pe2Configs[i];
        auto em = std::make_unique<particle::PlaneEmitter>();
        particle::ApplyInit(*em, particle::InitFromLegacyConfig(pcfg));
        rs_.Particles().AddPlaneEmitter(mi->handle, i, std::move(em));
    }
    mi->render.pe2State.resize(tmpl->pe2Configs.size());

    for (i32 i= 0; i < (i32)tmpl->ribbonConfigs.size(); i++)
        mi->render.ribbons.AddEmitter(i, tmpl->ribbonConfigs[i]);

    for (i32 i= 0; i < (i32)tmpl->pe1Configs.size(); i++)
        mi->render.pe1.AddEmitter(i, tmpl->pe1Configs[i]);

    // CornFx (CornEmitter) — register one emitter per init in the
    // service's per-(actor, emitterId) map. The emitter loads its .pkb
    // asset through CornEffectsAssetCache (deduped by path); per-frame state
    // flows through FrameState::cornStates → ApplyCornFrameStates.
    const Vector4f teamRGBA = {
        ((mi->teamColor       ) & 0xFF) / 255.0f,
        ((mi->teamColor >>  8 ) & 0xFF) / 255.0f,
        ((mi->teamColor >> 16 ) & 0xFF) / 255.0f,
        1.0f,
    };
    for (const auto& cinit : tmpl->cornEmitterInits) {
        if (cinit.pkbPath.empty()) continue;
        auto em = std::make_unique<corn_effects::CornEffectsEmitter>(
            rs_.CornEffects().Cache(),
            cinit.pkbPath,
            cinit.animVisibilityGuide,
            cinit.replaceableId,
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
    if (!mi) return;

    for (auto& tex : textures) {
        StagedTexture& st = mi->render.stagedTextures[tex.textureId];
        st.width  = tex.width;
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
        sm.layers        = mat.layers;
        sm.priorityPlane = mat.priorityPlane;
        sm.sortOrder     = mat.sortOrder;
    }

    mi->render.stagedDirty = true;
}


u32 ModelLoader::AddModel(const std::vector<MeshData>& meshes,
                          const std::vector<TextureData>& textures,
                          const std::vector<MaterialData>& materials,
                          const SkeletonData& skeleton,
                          const std::vector<SkinWeightData>& skinWeights,
                          const std::vector<ParticleEmitterConfig>& particleConfigs,
                          const std::vector<RibbonEmitterConfig>& ribbonConfigs,
                          const std::vector<CollisionShapeData>& collisions) {
    u32 handle = rs_.Scene().AllocActorId();
    auto mi = std::make_unique<Actor>();
    mi->handle = handle;

    for (auto& tex : textures) {
        StagedTexture& st = mi->render.stagedTextures[tex.textureId];
        st.width  = tex.width;
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
        sm.layers        = mat.layers;
        sm.priorityPlane = mat.priorityPlane;
        sm.sortOrder     = mat.sortOrder;
    }

    for (auto& mesh : meshes) {
        StagedGeoset& sg = mi->render.stagedGeosets[mesh.geosetId];
        sg.materialId = mesh.materialId;
        sg.lod        = mesh.lod;
        i32 vc = (i32)mesh.positions.size();
        sg.vertices.resize(vc);
        for (i32 i= 0; i < vc; i++) {
            sg.vertices[i].position = mesh.positions[i];
            sg.vertices[i].normal   = (i < (i32)mesh.normals.size()) ? mesh.normals[i] : Vector3f{0,0,1};
            sg.vertices[i].uv       = (i < (i32)mesh.uvs.size()) ? mesh.uvs[i] : Vector2f{0,0};
            sg.vertices[i].color    = {1.0f, 1.0f, 1.0f, 1.0f};
        }
        if ((i32)mesh.tangents.size() == vc) sg.tangents = mesh.tangents;
        sg.indices = mesh.indices;
    }

    if (skeleton.nodeCount > 0) {

        std::vector<f32> invBindFlat(skeleton.nodeCount * 16);
        for (i32 i= 0; i < skeleton.nodeCount; i++) {
            memcpy(&invBindFlat[i * 16], &skeleton.inverseBindMatrices[i].data[0][0], 64);
        }
        mi->render.skinning.SetSkeleton(skeleton.nodeCount, invBindFlat.data());
        mi->render.billboardFlags = skeleton.billboardFlags;
        mi->render.nodePivots         = skeleton.nodePivots;
        mi->render.nodeParents        = skeleton.nodeParents;
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
    auto paletteDecision = animation::DecidePaletteLayoutAndRewrite(
        skeleton.nodeCount, rewrittenSkinWeights);
    if (auto data = mi->render.skinning.SharedData()) {
        // SharedData was set by SetSkeleton above (which calls
        // ensureOwnedData internally) — apply the decision to the
        // owned instance.
        data->actorPaletteSize     = paletteDecision.actorPaletteSize;
        data->usesPerActorPalette  = paletteDecision.usesPerActorPalette;
        data->globalGroupAverages  = std::move(paletteDecision.globalGroupAverages);
    }

    for (auto& sw : rewrittenSkinWeights) {
        i32 vc = (i32)sw.influences.size();
        std::vector<i32>   boneIdx(vc * 4);
        std::vector<f32> weights(vc * 4);
        for (i32 v= 0; v < vc; v++) {
            for (i32 j= 0; j < 4; j++) {
                boneIdx[v * 4 + j] = sw.influences[v].boneIdx[j];
                weights[v * 4 + j] = sw.influences[v].weight[j];
            }
        }
        mi->render.skinning.SetGeosetWeights(sw.geosetId, vc, boneIdx.data(), weights.data());
        GeosetPaletteLayout layout;
        layout.subsetNodeIndices = sw.subsetNodeIndices;
        layout.groupAverages     = sw.groupAverages;
        mi->render.skinning.SetGeosetLayout(sw.geosetId, std::move(layout));
    }
    if (!skinWeights.empty()) mi->render.skinDirty = true;

    for (usize i = 0; i < particleConfigs.size(); i++) {
        const auto& pcfg = particleConfigs[i];
        auto em = std::make_unique<particle::PlaneEmitter>();
        particle::ApplyInit(*em, particle::InitFromLegacyConfig(pcfg));
        rs_.Particles().AddPlaneEmitter(handle, (i32)i, std::move(em));
    }
    mi->render.pe2State.resize(particleConfigs.size());

    for (usize i = 0; i < ribbonConfigs.size(); i++) {
        mi->render.ribbons.AddEmitter((i32)i, ribbonConfigs[i]);
    }

    for (auto& cs : collisions) {
        CollisionShape shape;
        shape.type   = cs.type;
        shape.vmin   = cs.vertices[0];
        shape.vmax   = cs.vertices[1];
        shape.radius = cs.radius;
        shape.pivot  = cs.pivot;
        mi->render.collisionShapes.push_back(shape);
    }

    mi->render.stagedDirty = true;
    rs_.Scene().Actors().All()[handle] = std::move(mi);
    return handle;
}

u32 ModelLoader::AddModelByPath(const std::string& mdxPath,
                                const Matrix44f&   initialTm) {

    auto tmpl = rs_.Scene().Templates().GetOrLoadSync(mdxPath);
    if (!tmpl) return 0;

    u32 handle;
    {
        handle = rs_.Scene().AllocActorId();
        auto mi = std::make_unique<Actor>();
        mi->handle         = handle;
        mi->worldTransform = initialTm;
        StageActor(mi.get(), tmpl);
        for (auto& cs : tmpl->collisionConfigs) {
            CollisionShape shape;
            shape.type   = cs.type;
            shape.vmin   = cs.vertices[0];
            shape.vmax   = cs.vertices[1];
            shape.radius = cs.radius;
            shape.pivot  = cs.pivot;
            mi->render.collisionShapes.push_back(shape);
        }
        rs_.Scene().Actors().All()[handle] = std::move(mi);
    }

    if (!tmpl->attachmentConfigs.empty())
        SetAttachmentConfigs(handle, tmpl->attachmentConfigs);

    return handle;
}

Actor* ModelLoader::SpawnUnit(const std::string& mdxPath,
                              const Matrix44f&   initialTm) {
    const u32 h = AddModelByPath(mdxPath, initialTm);
    if (h == 0) return nullptr;
    return rs_.Scene().Actors().Find(h);
}

Actor* ModelLoader::SpawnUnitFromSource(std::shared_ptr<IModelSource> source,
                                        const Matrix44f& initialTm) {
    if (!source) return nullptr;

    source->SetTextureCacheQuery(
        [this](std::string_view k) { return IsTextureCached(k); });

    ModelData data = source->Build();
    const u32 h = AddModel(data.meshes, data.textures, data.materials,
                                data.skeleton, data.skinWeights,
                                data.pe2Configs, data.ribbonConfigs,
                                data.collisionConfigs);
    Actor* actor = rs_.Scene().Actors().Find(h);
    if (!actor) return nullptr;

    actor->worldTransform = initialTm;
    actor->animation.Bind(source);

    if (!data.attachmentConfigs.empty())
        SetAttachmentConfigs(h, data.attachmentConfigs);
    if (!data.pe1Configs.empty())
        SetPE1Configs(h, data.pe1Configs);

    return actor;
}

bool ModelLoader::IsTextureCached(std::string_view key) const {
    return rs_.HasCachedTexture(key);
}

void ModelLoader::UploadStagedTextures(Actor& mi) {
    if (!mi.render.textures) mi.render.textures = rs_.Textures().CreateModelScope();
    for (auto& [id, st] : mi.render.stagedTextures) {

        if (!st.sharedKey.empty() && st.pixels.empty()) {

            if (mi.render.textures->BindShared(id, st.sharedKey, st.wrapFlags)
                == gfx::TextureHandle::Invalid) {
                std::string msg = "[WDEX texture] eviction race for '";
                msg += st.sharedKey;
                msg += "' — using fallback\n";
                DbgPrint(msg.c_str());
            }
            continue;
        }
        if (st.width <= 0 || st.height <= 0) continue;

        const gfx::Format texFormat = (st.format == gfx::Format::Unknown)
                                          ? gfx::Format::R8G8B8A8_UNORM
                                          : st.format;
        const gfx::TextureDesc desc{
            .width     = st.width,
            .height    = st.height,
            .mipLevels = (std::max)(1, st.mipLevels),
            .format    = texFormat,
            .usage     = gfx::TextureUsage::ShaderResource,
        };
        if (st.sharedKey.empty()) {
            mi.render.textures->Upload(id, desc, st.pixels.data(), st.wrapFlags);
        } else {
            mi.render.textures->UploadShared(id, st.sharedKey, desc,
                                      st.pixels.data(), st.wrapFlags);
        }
    }
    mi.render.stagedTextures.clear();
}

void ModelLoader::uploadTemplateGpu(ModelTemplate& tmpl) {
    if (tmpl.gpuUploaded) return;
    tmpl.sharedGeosets.clear();
    tmpl.sharedGeosets.reserve(tmpl.meshes.size());

    std::unordered_map<i32, const SkinWeightData*> weightsByGeoset;
    weightsByGeoset.reserve(tmpl.skinWeights.size());
    for (const auto& sw : tmpl.skinWeights) weightsByGeoset[sw.geosetId] = &sw;

    for (const auto& mesh : tmpl.meshes) {
        ModelTemplate::SharedGeoset sg;
        sg.geosetId    = mesh.geosetId;
        sg.materialId  = mesh.materialId;
        sg.lod         = mesh.lod;
        sg.vertexCount = (i32)mesh.positions.size();
        sg.indexCount  = (i32)mesh.indices.size();

        std::vector<Vertex> vertices(sg.vertexCount);
        for (i32 i= 0; i < sg.vertexCount; i++) {
            vertices[i].position = mesh.positions[i];
            vertices[i].normal   = (i < (i32)mesh.normals.size()) ? mesh.normals[i] : Vector3f{0,0,1};
            vertices[i].uv       = (i < (i32)mesh.uvs.size())     ? mesh.uvs[i]     : Vector2f{0,0};
            vertices[i].color    = {1.0f, 1.0f, 1.0f, 1.0f};
        }
        sg.unskinnedVb = rs_.Pipeline().Gfx()->CreateBuffer({
            .size  = (u32)(sizeof(Vertex) * sg.vertexCount),
            .usage = gfx::BufferUsage::Vertex,
        }, vertices.data());

        const bool hasUv1Data =
            (i32)mesh.uvs1.size() == sg.vertexCount && sg.vertexCount > 0;
        bool wantsUv1 = false;
        if (hasUv1Data && mesh.materialId >= 0) {
            for (const auto& mat : tmpl.materials) {
                if (mat.materialId != mesh.materialId) continue;
                for (const auto& lay : mat.layers) {
                    if (lay.coordId == 1) { wantsUv1 = true; break; }
                }
                break;
            }
        }
        if (wantsUv1) {
            std::vector<Vertex> verticesUv1(sg.vertexCount);
            for (i32 i= 0; i < sg.vertexCount; i++) {
                verticesUv1[i].position = mesh.positions[i];
                verticesUv1[i].normal   = (i < (i32)mesh.normals.size())
                                          ? mesh.normals[i] : Vector3f{0, 0, 1};
                verticesUv1[i].uv       = mesh.uvs1[i];
                verticesUv1[i].color    = {1.0f, 1.0f, 1.0f, 1.0f};
            }
            sg.unskinnedVb1 = rs_.Pipeline().Gfx()->CreateBuffer({
                .size  = (u32)(sizeof(Vertex) * sg.vertexCount),
                .usage = gfx::BufferUsage::Vertex,
            }, verticesUv1.data());
        }

        sg.ib = rs_.Pipeline().Gfx()->CreateBuffer({
            .size  = (u32)(sizeof(u32) * sg.indexCount),
            .usage = gfx::BufferUsage::Index,
        }, mesh.indices.data());

        if ((i32)mesh.tangents.size() == sg.vertexCount) {
            sg.tangentVb = rs_.Pipeline().Gfx()->CreateBuffer({
                .size  = (u32)(sizeof(Vector4f) * sg.vertexCount),
                .usage = gfx::BufferUsage::Vertex,
            }, mesh.tangents.data());
        }

        auto wIt = weightsByGeoset.find(mesh.geosetId);
        if (wIt != weightsByGeoset.end()
            && (i32)wIt->second->influences.size() == sg.vertexCount) {
            const auto& sw = *wIt->second;
            std::vector<BoneVertex> bv(sg.vertexCount);
            for (i32 v= 0; v < sg.vertexCount; v++) {
                const auto& inf = sw.influences[v];
                i32   idxArr[4] = { inf.boneIdx[0], inf.boneIdx[1], inf.boneIdx[2], inf.boneIdx[3] };
                f32 wtArr[4]  = { inf.weight[0],  inf.weight[1],  inf.weight[2],  inf.weight[3]  };
                bls::PackBoneVertex(bv[v], idxArr, wtArr);
            }
            sg.boneVb = rs_.Pipeline().Gfx()->CreateBuffer({
                .size  = (u32)(sizeof(BoneVertex) * sg.vertexCount),
                .usage = gfx::BufferUsage::Vertex,
            }, bv.data());
        }

        tmpl.sharedGeosets.push_back(sg);
    }

    if (!tmpl.templateTextures) tmpl.templateTextures = rs_.Textures().CreateModelScope();
    for (auto& tex : tmpl.textures) {
        if (tex.sharedKey.empty()) continue;
        if (tex.replaceableId != 0) continue;
        if (!tex.pixels.empty() && tex.width > 0 && tex.height > 0) {
            const gfx::Format texFormat = (tex.format == gfx::Format::Unknown)
                                              ? gfx::Format::R8G8B8A8_UNORM
                                              : tex.format;
            const gfx::TextureDesc desc{
                .width     = tex.width,
                .height    = tex.height,
                .mipLevels = (std::max)(1, tex.mipLevels),
                .format    = texFormat,
                .usage     = gfx::TextureUsage::ShaderResource,
            };
            tmpl.templateTextures->UploadShared(tex.textureId, tex.sharedKey,
                                                desc, tex.pixels.data(),
                                                tex.wrapFlags);
        } else {

            tmpl.templateTextures->BindShared(tex.textureId, tex.sharedKey,
                                              tex.wrapFlags);
        }
    }
    tmpl.gpuUploaded = true;
}

void ModelLoader::UploadStagedGeosets(Actor& mi) {
    if (mi.sourceTemplate) {

        auto& tmpl = *mi.sourceTemplate;
        uploadTemplateGpu(tmpl);
        if (mi.render.gpuGeosets.empty()) {

            for (const auto& shared : tmpl.sharedGeosets) {
                GPUGeoset gg;
                gg.geosetId    = shared.geosetId;
                gg.materialId  = shared.materialId;
                gg.lod         = shared.lod;
                gg.ib          = shared.ib;
                gg.unskinnedVb  = shared.unskinnedVb;
                gg.unskinnedVb1 = shared.unskinnedVb1;
                gg.tangentVb   = shared.tangentVb;
                gg.boneVb      = shared.boneVb;
                gg.indexCount  = shared.indexCount;
                gg.vertexCount = shared.vertexCount;
                gg.hasSkinning = true;
                if (shared.materialId >= 0 && shared.materialId < (i32)mi.render.gpuMaterials.size())
                    gg.priorityPlane = mi.render.gpuMaterials[shared.materialId].cpu.priorityPlane;
                mi.render.gpuGeosets.push_back(gg);
            }
        } else {

            for (auto& gg : mi.render.gpuGeosets) {
                if (gg.materialId >= 0 && gg.materialId < (i32)mi.render.gpuMaterials.size())
                    gg.priorityPlane = mi.render.gpuMaterials[gg.materialId].cpu.priorityPlane;
            }
        }
        mi.render.stagedGeosets.clear();
    } else {

        for (auto& [id, sg] : mi.render.stagedGeosets) {
            GPUGeoset gg;
            gg.geosetId    = id;
            gg.materialId  = sg.materialId;
            gg.lod         = sg.lod;
            gg.indexCount   = (i32)sg.indices.size();
            gg.vertexCount  = (i32)sg.vertices.size();
            gg.hasSkinning  = true;

            if (sg.materialId >= 0 && sg.materialId < (i32)mi.render.gpuMaterials.size())
                gg.priorityPlane = mi.render.gpuMaterials[sg.materialId].cpu.priorityPlane;

            const u32 vbBytes = (u32)(sizeof(Vertex) * sg.vertices.size());
            const GeosetSkinInfo* skinInfo = mi.render.skinning.GetGeosetWeights(id);

            gg.unskinnedVb = rs_.Pipeline().Gfx()->CreateBuffer({
                .size  = vbBytes,
                .usage = gfx::BufferUsage::Vertex,
            }, sg.vertices.data());

            gg.ib = rs_.Pipeline().Gfx()->CreateBuffer({
                .size  = (u32)(sizeof(u32) * sg.indices.size()),
                .usage = gfx::BufferUsage::Index,
            }, sg.indices.data());

            if ((i32)sg.tangents.size() == gg.vertexCount) {
                gg.tangentVb = rs_.Pipeline().Gfx()->CreateBuffer({
                    .size  = (u32)(sizeof(Vector4f) * sg.tangents.size()),
                    .usage = gfx::BufferUsage::Vertex,
                }, sg.tangents.data());
            }

            if (skinInfo && (i32)skinInfo->vertices.size() == gg.vertexCount) {
                std::vector<BoneVertex> bv(gg.vertexCount);
                for (i32 v= 0; v < gg.vertexCount; v++) {
                    const auto& inf = skinInfo->vertices[v];
                    i32   idxArr[4] = { inf.boneIdx[0], inf.boneIdx[1], inf.boneIdx[2], inf.boneIdx[3] };
                    f32 wtArr[4]  = { inf.weight[0],  inf.weight[1],  inf.weight[2],  inf.weight[3]  };
                    bls::PackBoneVertex(bv[v], idxArr, wtArr);
                }
                gg.boneVb = rs_.Pipeline().Gfx()->CreateBuffer({
                    .size  = (u32)(sizeof(BoneVertex) * gg.vertexCount),
                    .usage = gfx::BufferUsage::Vertex,
                }, bv.data());
            }

            mi.render.gpuGeosets.push_back(gg);
        }
        mi.render.stagedGeosets.clear();
    }

    mi.render.hasLods = false;
    for (const auto& g : mi.render.gpuGeosets) {
        if (g.lod != 0 && g.lod != 0xFFFFFFFFu) { mi.render.hasLods = true; break; }
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
        if (skinning.ActorPaletteCb() == gfx::BufferHandle::Invalid
            && skinning.ActorPaletteSize() > 0) {
            gfx::BufferHandle cb = rs_.Pipeline().Gfx()->CreateBuffer({
                .size  = sizeof(bls::BonePaletteCb),
                .usage = gfx::BufferUsage::Constant | gfx::BufferUsage::CpuWritable,
            });
            skinning.SetActorPaletteCb(cb);
        }
        for (auto& geo : mi.render.gpuGeosets) {
            if (geo.boneVb == gfx::BufferHandle::Invalid) continue;
            geo.hasSkinning = true;
            // geo.bonePaletteCb stays Invalid; draw path falls back to
            // skinning.ActorPaletteCb() — see render_pipeline.cpp.
        }
        return;
    }

    // Path B (fallback): one CB per geoset, unchanged from the
    // pre-Path-A behavior. Used when the actor's palette size would
    // exceed kActorPaletteCap (rare — typically very large WoW rigs).
    for (auto& geo : mi.render.gpuGeosets) {
        if (geo.boneVb == gfx::BufferHandle::Invalid) continue;
        if (geo.bonePaletteCb != gfx::BufferHandle::Invalid) continue;
        if (skinning.GeosetPaletteSize(geo.geosetId) <= 0) continue;
        geo.bonePaletteCb = rs_.Pipeline().Gfx()->CreateBuffer({
            .size  = sizeof(bls::BonePaletteCb),
            .usage = gfx::BufferUsage::Constant | gfx::BufferUsage::CpuWritable,
        });
        geo.hasSkinning = true;
    }
}

void ModelLoader::CommitPendingUploads() {
    // Reap actors flagged by RequestClearAll(). Collect handles first to avoid mutating
    // the actor map while iterating.
    std::vector<u32> toReap;
    for (auto& [h, mi] : rs_.Scene().Actors().All())
        if (mi->render.stagedClear) toReap.push_back(h);
    for (u32 h : toReap) DestroyActor(h);

    for (auto& [h, miPtr] : rs_.Scene().Actors().All()) {
        auto* mi = miPtr.get();
        if (!mi->render.stagedDirty && !mi->render.skinDirty) continue;

        if (mi->render.stagedDirty) {
            UploadStagedTextures(*mi);

            for (auto& [id, sm] : mi->render.stagedMaterials) {
                if ((i32)mi->render.gpuMaterials.size() <= id) mi->render.gpuMaterials.resize(id + 1);
                mi->render.gpuMaterials[id].cpu = sm;
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

}
