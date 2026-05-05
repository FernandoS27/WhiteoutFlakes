#include "renderer/render_service.h"
#include "render_service_internal.h"
#include "render_pass.h"
#include "debug/debug_renderer.h"
#include "constants.h"
#include "renderer/sampler_asset_manager.h"
#include "renderer/texture_asset_manager.h"
#include "renderer/replaceable_texture_manager.h"
#include "renderer/scene_manager.h"
#include "renderer/model_template.h"
#include "renderer/model_template_manager.h"
#include "compiled_shaders.h"
#include "team_glow_data.h"
#include "mdx_model_adapter.h"
#include "file_content_provider.h"
#include "viewcube_atlas.h"
#include "bls/bls_shader_cache.h"
#include "bls/bls_program.h"
#include "bls/bls_pso_builder.h"
#include "bls/bls_mat_params.h"
#include "bls/bls_cb_layout.h"
#include "bls/bls_frame.h"
#include "bls/bls_draw_helpers.h"
#include "bls/scoped_cb.h"
#include "ibl/split_sum.h"
#include "ibl/env_probe.h"
#include "shadow/shadow_pass.h"
#include "model_source_utils.h"
#include "io/texture_image_usage.h"
#include <whiteout/models/mdx/parser.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <numbers>
#include <cstring>

#if defined(_WIN32)
extern "C" __declspec(dllimport) void __stdcall OutputDebugStringA(const char* s);
#else
inline void OutputDebugStringA(const char*) {}
#endif

namespace WhiteoutDex {

RenderService::RenderService()
    : ownedScene_(std::make_unique<SceneManager>()),
      scene_(ownedScene_.get()),
      debug_(std::make_unique<DebugRenderer>(*this)),
      spnSpawner_(std::make_unique<SpnSpawner>(*this)),
      soundEmitter_(MakeNullSoundEmitter()) {

    scene_->Templates().SetTextureCacheQuery(
        [this](std::string_view k) { return IsTextureCached(k); });
}

RenderService::RenderService(SceneManager& scene)
    : scene_(&scene),
      debug_(std::make_unique<DebugRenderer>(*this)),
      spnSpawner_(std::make_unique<SpnSpawner>(*this)),
      soundEmitter_(MakeNullSoundEmitter()) {

    scene_->Templates().SetTextureCacheQuery(
        [this](std::string_view k) { return IsTextureCached(k); });
}

RenderService::~RenderService() = default;

Actor* RenderService::focusModel() const { return scene_->FocusActor(); }
Actor* RenderService::getModel(u32 h) const { return scene_->Actors().Find(h); }

void RenderService::SetIgnoreNonLooping(bool on) {
    ignoreNonLooping_ = on;

    std::lock_guard<std::mutex> lock(dataMutex_);
    for (auto& [h, mi] : scene_->Actors().All()) {
        if (mi->isPE1Child) continue;
        mi->ignoreNonLooping = on;
    }
}

void RenderService::ClearModel() {
    std::lock_guard<std::mutex> lock(dataMutex_);
    for (auto& [h, mi] : scene_->Actors().All()) {
        mi->render.stagedClear = true;
        mi->render.stagedDirty = true;
    }
    scene_->FocusRef() = 0;

    particleService_.Clear();

    splatService_.Clear();
    if (spnSpawner_) spnSpawner_->Clear();
}

void RenderService::SetAttachmentConfigs(u32 handle, const std::vector<AttachmentConfig>& configs) {
    std::lock_guard<std::mutex> lock(dataMutex_);
    auto* mi = getModel(handle);
    if (!mi) return;
    mi->attachmentSlots.clear();

    for (auto& cfg : configs) {
        Actor::AttachmentSlot slot;
        slot.config = cfg;
        slot.loaded = cfg.modelPath.empty();
        mi->attachmentSlots.push_back(slot);
    }
}

void RenderService::SetPE1Configs(u32 handle, const std::vector<PE1EmitterConfig>& configs) {
    std::lock_guard<std::mutex> lock(dataMutex_);
    auto* mi = getModel(handle);
    if (!mi) return;
    for (i32 i= 0; i < (i32)configs.size(); i++)
        mi->render.pe1.AddEmitter(i, configs[i]);
}

void RenderService::UpdateAttachments() {
    std::lock_guard<std::mutex> lock(dataMutex_);

    std::vector<u32> handles;
    for (auto& [h, mi] : scene_->Actors().All()) handles.push_back(h);

    for (u32 h : handles) {
        auto* mi = getModel(h);
        if (!mi) continue;

        for (auto& slot : mi->attachmentSlots) {
            if (slot.loaded || slot.config.modelPath.empty()) continue;

            auto tmpl = scene_->Templates().GetOrLoadAsync(slot.config.modelPath);
            if (!tmpl) continue;

            slot.loaded = true;

            u32 childH = scene_->NextActorIdRef()++;
            auto child = std::make_unique<Actor>();
            child->handle = childH;
            child->parent = mi->handle;
            child->isPE1Child = true;
            child->pe1Depth = mi->pe1Depth + 1;
            child->animation.Bind(tmpl->adapter);
            child->animation.SetBirthTimeMs(scene_->GetAnimationTime());

            auto seqs = tmpl->adapter->GetSequences();
            if (!seqs.empty())
                child->animation.SetActiveSequenceIndex(rand() % (i32)seqs.size());

            stageModelFromTemplate(child.get(), tmpl);
            scene_->Actors().All()[childH] = std::move(child);
            slot.childModelHandle = childH;
        }
    }
}

void RenderService::stageModelFromTemplate(Actor* mi,
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
        if (tex.replaceableId != 0 && replaceables_)
            replaceables_->RegisterModelSlot(*mi, tex.textureId, tex.replaceableId);
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
        particleService_.AddPlaneEmitter(mi->handle, i, std::move(em));
    }
    mi->render.pe2State.resize(tmpl->pe2Configs.size());

    for (i32 i= 0; i < (i32)tmpl->ribbonConfigs.size(); i++)
        mi->render.ribbons.AddEmitter(i, tmpl->ribbonConfigs[i]);

    for (i32 i= 0; i < (i32)tmpl->pe1Configs.size(); i++)
        mi->render.pe1.AddEmitter(i, tmpl->pe1Configs[i]);

    mi->events.Reset(tmpl->eventObjects, tmpl->globalSequences);

    mi->render.stagedDirty = true;
}

void RenderService::UpdatePE1(f32 dt) {
    std::lock_guard<std::mutex> lock(dataMutex_);
    std::vector<u32> toRemove;

    std::vector<u32> handles;
    for (auto& [h, mi] : scene_->Actors().All()) handles.push_back(h);

    for (u32 h : handles) {
        auto* mi = getModel(h);
        if (!mi) continue;
        if (mi->pe1Depth >= SceneManager::kMaxPE1Depth) continue;
        if (!mi->render.pe1.HasEmitters()) continue;
        if (mi->parentVisibility <= 0.02f) continue;

        auto result = mi->render.pe1.Simulate(dt, scene_->NextActorIdRef());

        for (auto& birth : result.born) {
            if (scene_->PE1InstanceCountRef() >= SceneManager::kMaxPE1Instances) continue;
            auto* cfg = mi->render.pe1.GetConfig(birth.emitterId);
            if (!cfg) continue;
            auto tmpl = scene_->Templates().GetOrLoadAsync(cfg->modelPath);
            if (!tmpl) continue;

            auto child = std::make_unique<Actor>();
            child->handle = birth.handle;
            child->parent = mi->handle;
            child->worldTransform = birth.worldTransform;
            child->isPE1Child = true;
            child->pe1Depth = mi->pe1Depth + 1;
            child->animation.Bind(tmpl->adapter);
            child->animation.SetBirthTimeMs(scene_->GetAnimationTime());

            stageModelFromTemplate(child.get(), tmpl);
            scene_->Actors().All()[birth.handle] = std::move(child);
            scene_->PE1InstanceCountRef()++;
        }

        for (u32 childH : result.died) toRemove.push_back(childH);

        for (auto& [childH, tm] : result.transforms) {
            if (auto* c = getModel(childH)) c->worldTransform = tm;
        }
    }

    for (u32 rh : toRemove) {
        auto it = scene_->Actors().All().find(rh);
        if (it != scene_->Actors().All().end()) {
            if (replaceables_) replaceables_->UnregisterModel(*it->second);
            it->second->ReleaseGPU(*gfx_);
            scene_->Actors().All().erase(it);
            scene_->PE1InstanceCountRef()--;
        }

        particleService_.RemoveModel(rh);
    }

    if (spnSpawner_) spnSpawner_->Tick(scene_->GetAnimationTime());
}

void RenderService::EvaluateTopLevelActors() {

    struct ActorEval {
        u32 handle;
        std::shared_ptr<IAnimationSource> adapter;
        Matrix44f worldTransform;
        i32 seqIdx;
        i32 localTimeMs;
        i32 globalTimeMs;
    };
    std::vector<ActorEval> toEval;
    Vector3f camPos;

    {
        std::lock_guard<std::mutex> lock(dataMutex_);
        camPos = scene_->Camera().GetSource();
        const i32 now= scene_->GetAnimationTime();
        for (auto& [h, mi] : scene_->Actors().All()) {
            if (mi->isPE1Child) continue;
            if (mi->externallyDriven) continue;
            if (!mi->animation.HasSource()) continue;
            const i32 localTime  = mi->animation.TimeMs();
            const i32 globalTime = now - mi->animation.BirthTimeMs();
            toEval.push_back({h, mi->animation.Source(), mi->worldTransform,
                              mi->animation.ActiveSequenceIndex(),
                              localTime, globalTime});
        }
    }

    for (auto& ae : toEval) {
        FrameState fs = ae.adapter->Evaluate(ae.seqIdx, ae.localTimeMs, ae.globalTimeMs,
                                             ae.worldTransform, camPos);
        ApplyFrameState(ae.handle, fs, ae.localTimeMs);
    }
}

void RenderService::EvaluatePE1Children() {
    struct ChildEval {
        u32 handle;
        std::shared_ptr<IAnimationSource> adapter;
        i32 localTimeMs;
        i32 seqIdx;
        i32 globalTimeMs;
        Matrix44f worldTransform;

    };
    std::vector<ChildEval> toEval;
    Vector3f camPos;

    {
        std::lock_guard<std::mutex> lock(dataMutex_);
        camPos = scene_->Camera().GetSource();
        i32 timeMs = scene_->GetAnimationTime();
        for (auto& [h, mi] : scene_->Actors().All()) {
            if (!mi->isPE1Child || !mi->animation.HasSource()) continue;
            if (mi->parentVisibility <= 0.02f) continue;
            i32 localTime = timeMs - mi->animation.BirthTimeMs();
            if (localTime < 0) localTime = 0;
            i32 globalTime = localTime;
            auto seqs = mi->animation.Sequences();
            const i32 seqIdx = mi->animation.ActiveSequenceIndex();
            if (!seqs.empty()) {
                const i32 boundedSeq = seqIdx % (i32)seqs.size();
                const auto& seq = seqs[boundedSeq];
                const i32 dur = seq.endMs - seq.startMs;
                if (dur > 0) {

                    if (seq.nonLooping && !mi->ignoreNonLooping)
                        localTime = seq.startMs + (std::min)(localTime, dur);
                    else
                        localTime = seq.startMs + (localTime % dur);
                }
            }
            toEval.push_back({h, mi->animation.Source(), localTime, seqIdx,
                              globalTime, mi->worldTransform});
        }
    }

    for (auto& ce : toEval) {
        FrameState fs = ce.adapter->Evaluate(ce.seqIdx, ce.localTimeMs, ce.globalTimeMs,
                                             ce.worldTransform, camPos);
        ApplyFrameState(ce.handle, fs, ce.localTimeMs);
    }
}

void RenderService::UpdateMaterials(u32 handle, const std::vector<MaterialData>& materials,
                               const std::vector<TextureData>& textures) {
    std::lock_guard<std::mutex> lock(dataMutex_);
    auto* mi = getModel(handle);
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
        if (tex.replaceableId != 0 && replaceables_)
            replaceables_->RegisterModelSlot(*mi, tex.textureId, tex.replaceableId);
    }

    for (auto& mat : materials) {
        StagedMaterial& sm = mi->render.stagedMaterials[mat.materialId];
        sm.layers        = mat.layers;
        sm.priorityPlane = mat.priorityPlane;
        sm.sortOrder     = mat.sortOrder;
    }

    mi->render.stagedDirty = true;
}

void RenderService::UpdateMaterials(const std::vector<MaterialData>& materials,
                               const std::vector<TextureData>& textures) {
    UpdateMaterials(scene_->FocusRef(), materials, textures);
}

u32 RenderService::AddModel(const std::vector<MeshData>& meshes,
                            const std::vector<TextureData>& textures,
                            const std::vector<MaterialData>& materials,
                            const SkeletonData& skeleton,
                            const std::vector<SkinWeightData>& skinWeights,
                            const std::vector<ParticleEmitterConfig>& particleConfigs,
                            const std::vector<RibbonEmitterConfig>& ribbonConfigs,
                            const std::vector<CollisionShapeData>& collisions) {
    std::lock_guard<std::mutex> lock(dataMutex_);
    u32 handle = scene_->NextActorIdRef()++;
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

        if (tex.replaceableId != 0 && replaceables_)
            replaceables_->RegisterModelSlot(*mi, tex.textureId, tex.replaceableId);
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

    for (auto& sw : skinWeights) {
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
        particleService_.AddPlaneEmitter(handle, (i32)i, std::move(em));
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
    if (scene_->FocusRef() == 0) scene_->FocusRef() = handle;
    scene_->Actors().All()[handle] = std::move(mi);
    return handle;
}

u32 RenderService::AddModelByPath(const std::string& mdxPath) {

    auto tmpl = scene_->Templates().GetOrLoadSync(mdxPath);
    if (!tmpl) return 0;

    u32 handle;
    {
        std::lock_guard<std::mutex> lock(dataMutex_);
        handle = scene_->NextActorIdRef()++;
        auto mi = std::make_unique<Actor>();
        mi->handle = handle;
        stageModelFromTemplate(mi.get(), tmpl);
        for (auto& cs : tmpl->collisionConfigs) {
            CollisionShape shape;
            shape.type   = cs.type;
            shape.vmin   = cs.vertices[0];
            shape.vmax   = cs.vertices[1];
            shape.radius = cs.radius;
            shape.pivot  = cs.pivot;
            mi->render.collisionShapes.push_back(shape);
        }
        scene_->Actors().All()[handle] = std::move(mi);
    }

    if (!tmpl->attachmentConfigs.empty())
        SetAttachmentConfigs(handle, tmpl->attachmentConfigs);

    return handle;
}

u32 RenderService::LoadModelByPath(const std::string& mdxPath) {
    ClearModel();
    u32 h = AddModelByPath(mdxPath);
    if (h == 0) return 0;

    {
        std::lock_guard<std::mutex> lock(dataMutex_);
        scene_->FocusRef() = h;
    }

    auto tmpl = scene_->Templates().Lookup(mdxPath);
    if (tmpl) {
        bool anyNonSd = false;
        for (auto& mat : tmpl->materials) {
            for (auto& layer : mat.layers) {
                if (layer.shaderId != 0) { anyNonSd = true; break; }
            }
            if (anyNonSd) break;
        }
        const RenderMode desired = anyNonSd ? RenderMode::HD : RenderMode::SD;
        if (renderMode_ != desired) {
            renderMode_ = desired;
            renderModeDirty_ = true;
        }
    }

    return h;
}

Actor* RenderService::SpawnActorFromMdx(const std::string& mdxPath) {
    const u32 h = AddModelByPath(mdxPath);
    if (h == 0) return nullptr;
    return scene_->Actors().Find(h);
}

Actor* RenderService::LoadActorFromMdx(const std::string& mdxPath) {
    const u32 h = LoadModelByPath(mdxPath);
    if (h == 0) return nullptr;
    Actor* actor = scene_->Actors().Find(h);

    if (actor) actor->ignoreNonLooping = ignoreNonLooping_;
    return actor;
}

Actor* RenderService::SpawnActorFromLiveSource(std::shared_ptr<IModelSource> source) {
    if (!source) return nullptr;

    source->SetTextureCacheQuery(
        [this](std::string_view k) { return IsTextureCached(k); });

    ModelData data = source->Build();
    const u32 h = AddModel(data.meshes, data.textures, data.materials,
                                data.skeleton, data.skinWeights,
                                data.pe2Configs, data.ribbonConfigs,
                                data.collisionConfigs);
    Actor* actor = scene_->Actors().Find(h);
    if (!actor) return nullptr;

    actor->animation.Bind(source);

    actor->externallyDriven = true;

    actor->ignoreNonLooping = ignoreNonLooping_;

    if (!data.attachmentConfigs.empty())
        SetAttachmentConfigs(h, data.attachmentConfigs);
    if (!data.pe1Configs.empty())
        SetPE1Configs(h, data.pe1Configs);

    bool anyNonSd = false;
    for (auto& mat : data.materials) {
        for (auto& layer : mat.layers) {
            if (layer.shaderId != 0) { anyNonSd = true; break; }
        }
        if (anyNonSd) break;
    }
    const RenderMode desired = anyNonSd ? RenderMode::HD : RenderMode::SD;
    if (renderMode_ != desired) {
        renderMode_ = desired;
        renderModeDirty_ = true;
    }

    return actor;
}

void RenderService::EvaluateAndApply(Actor& actor) {
    if (!actor.animation.HasSource()) return;
    Vector3f camPos;
    i32      globalTime;
    {
        std::lock_guard<std::mutex> lock(dataMutex_);
        camPos     = scene_->Camera().GetSource();
        globalTime = scene_->GetAnimationTime() - actor.animation.BirthTimeMs();
    }
    const i32 localTime = actor.animation.TimeMs();
    FrameState fs = actor.animation.Source()->Evaluate(
        actor.animation.ActiveSequenceIndex(), localTime, globalTime,
        actor.worldTransform, camPos);
    ApplyFrameState(actor.handle, fs, localTime);
}

void RenderService::ApplyBoneMatrices(Actor& mi, const FrameState& state) {
    if (state.boneWorldMatrices.empty()) return;

    i32 bc = (i32)state.boneWorldMatrices.size();
    Vector3f camPos = scene_->Camera().GetSource();

    const std::vector<u32>& billboardFlags = mi.sourceTemplate
        ? mi.sourceTemplate->skeleton.billboardFlags : mi.render.billboardFlags;
    const std::vector<Vector3f>& nodePivots = mi.sourceTemplate
        ? mi.sourceTemplate->skeleton.nodePivots : mi.render.nodePivots;
    const std::vector<i32>& nodeParents = mi.sourceTemplate
        ? mi.sourceTemplate->skeleton.nodeParents : mi.render.nodeParents;

    std::vector<f32> worldFlat(bc * 16);
    for (i32 i= 0; i < bc; i++) {
        Matrix44f boneM = state.boneWorldMatrices[i];

        u32 bbFlags = (i < (i32)billboardFlags.size()) ? billboardFlags[i] : 0;
        if (bbFlags != 0) {

            Vector3f pivF = (i < (i32)nodePivots.size())
                              ? nodePivots[i] : Vector3f{0, 0, 0};

            Vector3f pivWorld = whiteout::transform_point(pivF, boneM);

            if (bbFlags & BONE_BILLBOARD_CAMERA_ANCHORED) {

                i32 parentIdx = (i < (i32)nodeParents.size()) ? nodeParents[i] : -1;
                Vector3f parentWorld = {0, 0, 0};
                if (parentIdx >= 0 && parentIdx < (i32)state.boneWorldMatrices.size()) {
                    Vector3f parentPivF = (parentIdx < (i32)nodePivots.size())
                                            ? nodePivots[parentIdx] : Vector3f{0, 0, 0};
                    parentWorld = whiteout::transform_point(
                        parentPivF, state.boneWorldMatrices[parentIdx]);
                }
                Vector3f toCamDir = camPos - parentWorld;
                f32 camLen = toCamDir.length();
                if (camLen > kBillboardDistThreshold) {
                    toCamDir = toCamDir.normalized();
                    f32 restDist = (pivWorld - parentWorld).length();
                    pivWorld = parentWorld + Vector3f{toCamDir.x * restDist,
                                                      toCamDir.y * restDist,
                                                      toCamDir.z * restDist};
                }
            }

            Vector3f toCamera = camPos - pivWorld;
            f32 dist = toCamera.length();
            if (dist > kBillboardDistThreshold) {
                Vector3f toCam = toCamera.normalized();
                Vector3f worldUp = {0, 0, 1};

                auto rowToVec = [](const Matrix44f& m, i32 r) {
                    return Vector3f{m.data[r][0], m.data[r][1], m.data[r][2]};
                };

                const f32 sX= rowToVec(boneM, 0).length();
                const f32 sY= rowToVec(boneM, 1).length();
                const f32 sZ= rowToVec(boneM, 2).length();

                Matrix44f bbRot = Matrix44f::identity();
                bool haveRot = false;

                if (bbFlags & BONE_BILLBOARD_FULL) {

                    Vector3f xp = toCam;
                    Vector3f yp = {-xp.y, xp.x, 0.0f};
                    f32 yLen = yp.length();
                    if (yLen < kBillboardDistThreshold) yp = {0, 1, 0};
                    else                                yp = yp.normalized();
                    Vector3f zp = whiteout::cross(xp, yp);
                    bbRot = {};
                    bbRot.data[0][0] = xp.x; bbRot.data[0][1] = xp.y; bbRot.data[0][2] = xp.z;
                    bbRot.data[1][0] = yp.x; bbRot.data[1][1] = yp.y; bbRot.data[1][2] = yp.z;
                    bbRot.data[2][0] = zp.x; bbRot.data[2][1] = zp.y; bbRot.data[2][2] = zp.z;
                    bbRot.data[3][3] = 1.0f;
                    haveRot = true;
                } else if (bbFlags & BONE_BILLBOARD_LOCK_X) {

                    Vector3f xp = rowToVec(boneM, 0);
                    f32 xLen = xp.length();
                    if (xLen < kBillboardDistThreshold) xp = {1, 0, 0};
                    else                                xp = xp.normalized();
                    Vector3f zp = whiteout::cross(toCam, xp);
                    f32 zLen = zp.length();
                    if (zLen < kBillboardDistThreshold) zp = {0, 0, 1};
                    else                                zp = zp.normalized();
                    Vector3f yp = whiteout::cross(xp, zp);
                    bbRot = {};
                    bbRot.data[0][0] = xp.x; bbRot.data[0][1] = xp.y; bbRot.data[0][2] = xp.z;
                    bbRot.data[1][0] = yp.x; bbRot.data[1][1] = yp.y; bbRot.data[1][2] = yp.z;
                    bbRot.data[2][0] = zp.x; bbRot.data[2][1] = zp.y; bbRot.data[2][2] = zp.z;
                    bbRot.data[3][3] = 1.0f;
                    haveRot = true;
                } else if (bbFlags & BONE_BILLBOARD_LOCK_Y) {

                    Vector3f yp = rowToVec(boneM, 1);
                    f32 yLen = yp.length();
                    if (yLen < kBillboardDistThreshold) yp = {0, 1, 0};
                    else                                yp = yp.normalized();
                    Vector3f zp = whiteout::cross(toCam, yp);
                    f32 zLen = zp.length();
                    if (zLen < kBillboardDistThreshold) zp = {0, 0, 1};
                    else                                zp = zp.normalized();
                    Vector3f xp = whiteout::cross(yp, zp);
                    bbRot = {};
                    bbRot.data[0][0] = xp.x; bbRot.data[0][1] = xp.y; bbRot.data[0][2] = xp.z;
                    bbRot.data[1][0] = yp.x; bbRot.data[1][1] = yp.y; bbRot.data[1][2] = yp.z;
                    bbRot.data[2][0] = zp.x; bbRot.data[2][1] = zp.y; bbRot.data[2][2] = zp.z;
                    bbRot.data[3][3] = 1.0f;
                    haveRot = true;
                } else if (bbFlags & BONE_BILLBOARD_LOCK_Z) {

                    Vector3f zp = worldUp;
                    Vector3f yp = whiteout::cross(zp, toCam);
                    f32 yLen = yp.length();
                    if (yLen < kBillboardDistThreshold) yp = {0, 1, 0};
                    else                                yp = yp.normalized();
                    Vector3f xp = whiteout::cross(yp, zp);
                    bbRot = {};
                    bbRot.data[0][0] = xp.x; bbRot.data[0][1] = xp.y; bbRot.data[0][2] = xp.z;
                    bbRot.data[1][0] = yp.x; bbRot.data[1][1] = yp.y; bbRot.data[1][2] = yp.z;
                    bbRot.data[2][0] = zp.x; bbRot.data[2][1] = zp.y; bbRot.data[2][2] = zp.z;
                    bbRot.data[3][3] = 1.0f;
                    haveRot = true;
                }

                if (haveRot) {

                    bbRot.data[0][0] *= sX; bbRot.data[0][1] *= sX; bbRot.data[0][2] *= sX;
                    bbRot.data[1][0] *= sY; bbRot.data[1][1] *= sY; bbRot.data[1][2] *= sY;
                    bbRot.data[2][0] *= sZ; bbRot.data[2][1] *= sZ; bbRot.data[2][2] *= sZ;
                    Matrix44f T_negRest = Matrix44f::translation({-pivF.x, -pivF.y, -pivF.z});
                    Matrix44f T_world   = Matrix44f::translation({pivWorld.x, pivWorld.y, pivWorld.z});
                    boneM = T_negRest * bbRot * T_world;
                } else if (bbFlags & BONE_BILLBOARD_CAMERA_ANCHORED) {

                    Matrix44f S = {};
                    S.data[0][0] = sX; S.data[1][1] = sY; S.data[2][2] = sZ;
                    S.data[3][3] = 1.0f;
                    Matrix44f T_negRest = Matrix44f::translation({-pivF.x, -pivF.y, -pivF.z});
                    Matrix44f T_world   = Matrix44f::translation({pivWorld.x, pivWorld.y, pivWorld.z});
                    boneM = T_negRest * S * T_world;
                }
            }
        }

        memcpy(&worldFlat[i * 16], &boneM.data[0][0], 64);
    }
    mi.render.skinning.UpdateNodeMatrices(bc, worldFlat.data());
}

void RenderService::ApplyParticleFrameStates(Actor& mi, const FrameState& state) {

    for (usize i = 0; i < state.particleStates.size(); ++i) {
        const auto& ps = state.particleStates[i];
        auto* em = particleService_.GetEmitter(mi.handle, ps.emitterId);
        if (!em) continue;

        em->SetEmissionRate(ps.emissionRate);
        em->SetVelocity(ps.speed);
        em->SetVelocityVariation(ps.variation);
        em->SetLatitude(ps.coneAngle);
        em->SetAcceleration(ps.gravity);
        em->SetWidth(ps.width);
        em->SetHeight(ps.length);

        em->SetVisible(ps.visibility > 0.0f && !ps.squirting);
        if (ps.squirting) {
            auto& st = mi.render.pe2State[i];
            if (st.emissionValid) {
                if (ps.emissionRate > 0.02f && st.lastEmissionRate <= 0.02f)
                    em->SetSquirtPending(true);
            }
            st.lastEmissionRate = ps.emissionRate;
            st.emissionValid = true;
        }

        em->SetModelToWorld(CoordinateSystem::ConvertTransform(
            CoordinateSystem::Default(), em->GetCoordSpace(), ps.transform));
    }
}

void RenderService::ApplyAttachmentStates(Actor& mi, const FrameState& state, i32  ) {

    const i32 sceneNow = scene_->GetAnimationTime();
    for (auto& as : state.attachmentStates) {
        if (as.attachmentIndex < 0 || as.attachmentIndex >= (i32)mi.attachmentSlots.size()) continue;
        auto& slot = mi.attachmentSlots[as.attachmentIndex];
        if (slot.childModelHandle == 0) continue;
        auto* child = getModel(slot.childModelHandle);
        if (!child) continue;

        bool visible = (as.visibility > 0.0f);
        child->worldTransform = as.transform;

        if (visible && !slot.wasVisible) {
            child->animation.SetBirthTimeMs(sceneNow);
            auto seqs = child->animation.Sequences();
            if (!seqs.empty())
                child->animation.SetActiveSequenceIndex(rand() % (i32)seqs.size());
            slot.wasVisible = true;
        } else if (!visible) {
            slot.wasVisible = false;
        }

        child->parentVisibility = visible ? 1.0f : 0.0f;
    }
}

void RenderService::ApplyFrameState(u32 handle, const FrameState& state, i32 timeMs) {
    std::lock_guard<std::mutex> lock(dataMutex_);
    auto* mi = getModel(handle);
    if (!mi) return;

    ApplyBoneMatrices(*mi, state);
    mi->render.ApplyGeosetStates(state);
    mi->render.ApplyLayerStates(state);
    ApplyParticleFrameStates(*mi, state);
    mi->render.ApplyRibbonFrameStates(state);
    mi->render.ApplyPE1FrameStates(state);

    for (i32 i= 0; i < (i32)state.collisionTransforms.size() && i < (i32)mi->render.collisionShapes.size(); i++)
        mi->render.collisionShapes[i].transform = state.collisionTransforms[i];

    ApplyAttachmentStates(*mi, state, timeMs);

    if (showEvents_ && !mi->events.Empty()) {
        const i32 activeSeq = mi->animation.ActiveSequenceIndex();

        i32 seqStart = 0, seqEnd = 0x7FFFFFFF;
        if (mi->animation.Source()) {
            auto seqs = mi->animation.Source()->GetSequences();
            if (activeSeq >= 0 && activeSeq < (i32)seqs.size()) {
                seqStart = seqs[activeSeq].startMs;
                seqEnd   = seqs[activeSeq].endMs;
            }
        }
        const i32 globalMs = scene_->GetAnimationTime();
        mi->events.Tick(*mi,
                        state.boneWorldMatrices,
                        activeSeq,
                        timeMs, globalMs,
                        seqStart, seqEnd,
                        &splatService_,
                        spnSpawner_.get(),
                        soundEmitter_.get());
    }
}

bool RenderService::IsTextureCached(std::string_view key) const {
    return textures_ && textures_->IsCachedShared(key);
}

void RenderService::SetTeamColor(u8 r, u8 g, u8 b) {
    std::lock_guard<std::mutex> lock(dataMutex_);
    if (replaceables_) replaceables_->SetTeamColor(r, g, b);
}

void RenderService::SetTileset(io::Tileset ts) {
    std::lock_guard<std::mutex> lock(dataMutex_);
    if (replaceables_) replaceables_->SetTileset(ts);
}

io::Tileset RenderService::GetTileset() const {
    return io::GetCurrentTileset();
}

void RenderService::SetBackgroundColor(u8 r, u8 g, u8 b) {

    const u32 packed = (u32)r | ((u32)g << 8) | ((u32)b << 16);
    backgroundColor_.store(packed);
}

void RenderService::SetSoundEmitter(std::unique_ptr<ISoundEmitter> emitter) {

    std::lock_guard<std::mutex> lock(dataMutex_);
    soundEmitter_ = emitter ? std::move(emitter) : MakeNullSoundEmitter();

    soundEmitter_->SetVolume(soundVolume_);
}

void RenderService::SetSoundVolume(f32 v) {
    if (v < 0.0f) v = 0.0f;
    if (v > 1.0f) v = 1.0f;
    std::lock_guard<std::mutex> lock(dataMutex_);
    soundVolume_ = v;
    if (soundEmitter_) soundEmitter_->SetVolume(v);
}

void RenderService::ActivateCameraPreset(i32 idx) {
    std::lock_guard<std::mutex> lock(dataMutex_);
    const auto& presets = scene_->CameraPresets();
    if (idx < 0 || idx >= (i32)presets.size()) {
        scene_->Camera().SetOrbitalMode();
        scene_->Camera().SetFovDiagonal(Camera::kDefaultFovDiagonal);
        scene_->Camera().SetClip(Camera::kDefaultNearZ, Camera::kDefaultFarZ);
        scene_->SetActiveCameraPresetIdx(-1);
        return;
    }
    const auto& p = presets[idx];
    Vector3f pos  = p.position;
    Vector3f tgt  = p.target;
    f32      roll = p.staticRoll;

    if (p.animator) {
        i32 seqStart = 0, seqEnd = 0;

        Actor* focus  = scene_->FocusActor();
        i32    seqIdx = focus ? focus->animation.ActiveSequenceIndex() : 0;
        const auto& ranges = scene_->SequenceRanges();
        if (seqIdx >= 0 && seqIdx < (i32)ranges.size()) {
            seqStart = ranges[seqIdx].startMs;
            seqEnd   = ranges[seqIdx].endMs;
        }

        if (seqStart == 0 && seqEnd == 0) seqEnd = 1 << 30;

        const i32 sampleMs = focus ? focus->animation.TimeMs()
                                   : scene_->GetAnimationTime();
        p.animator(pos, tgt, roll, sampleMs, seqStart, seqEnd);
    }

    scene_->Camera().SetDirectPose(pos, tgt, roll);

    const f32 fov= (p.fovDiagonal > 1e-3f) ? p.fovDiagonal : Camera::kDefaultFovDiagonal;
    scene_->Camera().SetFovDiagonal(fov);
    scene_->Camera().SetClip(p.zNear, p.zFar);
    scene_->SetActiveCameraPresetIdx(idx);
}

std::optional<std::vector<CameraPreset>> RenderService::TakePendingCameraPresets() {
    std::lock_guard<std::mutex> lock(dataMutex_);
    return scene_->TakePendingCameraPresets();
}

i32 RenderService::GetActiveSequenceIndex() const {

    std::lock_guard<std::mutex> lock(dataMutex_);
    Actor* focus = scene_->FocusActor();
    return focus ? focus->animation.ActiveSequenceIndex() : 0;
}

void RenderService::SetActiveSequence(i32 i) {

    std::lock_guard<std::mutex> lock(dataMutex_);
    if (Actor* focus = scene_->FocusActor()) {
        focus->animation.SetActiveSequenceIndex(i);
    }
}

void RenderService::ClearSplats() {

    std::lock_guard<std::mutex> lock(dataMutex_);
    splatService_.Clear();
}

std::optional<std::vector<std::string>> RenderService::TakePendingSequences() {
    std::lock_guard<std::mutex> lock(dataMutex_);
    return scene_->TakePendingSequences();
}

void RenderService::RotateCamera(i32 dx, i32 dy) {
    std::lock_guard<std::mutex> lock(dataMutex_);
    scene_->Camera().Rotate(dx, dy);
}

void RenderService::PanCamera(i32 dx, i32 dy) {
    std::lock_guard<std::mutex> lock(dataMutex_);
    scene_->Camera().Pan(dx, dy);
}

void RenderService::ZoomCamera(i32 delta) {
    std::lock_guard<std::mutex> lock(dataMutex_);
    scene_->Camera().Zoom(delta);
}

void RenderService::ZoomCameraSmooth(i32 dy) {
    std::lock_guard<std::mutex> lock(dataMutex_);
    scene_->Camera().ZoomSmooth((f32)dy * scene_->Camera().GetDistance() / Camera::kFactorRelDist);
}

void RenderService::ResetCamera() {
    std::lock_guard<std::mutex> lock(dataMutex_);
    scene_->Camera().Reset();
}

void RenderService::SetDisplayFlags(const DisplayFlags& flags) {
    showGrid_       = flags.showGrid;
    showParticles_  = flags.showParticles;
    showRibbons_    = flags.showRibbons;
    showCollisions_ = flags.showCollisions;
    showLights_     = flags.showLights;
    showEvents_     = flags.showEvents;
    renderMode_     = flags.renderMode;
}

DisplayFlags RenderService::GetDisplayFlags() const {
    return { showGrid_, showParticles_, showRibbons_, showCollisions_, showLights_, showEvents_, renderMode_ };
}

void RenderService::Tick(f32 dt) {

    scene_->Templates().Tick();
    ProcessStagedData();
    UpdateAttachments();
    EvaluateTopLevelActors();
    EvaluatePE1Children();
    UpdateAnimation();
    UpdateParticles(dt);
    UpdatePE1(dt);
    UpdateRibbons(dt);
}

void RenderService::ShutdownDevice() {

    if (spnSpawner_) spnSpawner_->Clear();
    splatService_.Clear();
    ReleaseModelGPU();

    scene_->Templates().ReleaseAllGPU(*gfx_);
    scene_->Templates().Clear();
    CleanupD3D();
}

void RenderService::GetFrameStats(i32& geosets, i32& textures, i32& nodes,
                              i32& particles, i32& segments) const {
    geosets = textures = nodes = particles = segments = 0;
    std::lock_guard<std::mutex> lock(dataMutex_);
    for (auto& [h, mi] : scene_->Actors().All()) {
        geosets  += (i32)mi->render.gpuGeosets.size();
        textures += mi->render.textures ? (i32)mi->render.textures->Size() : 0;
        nodes    += mi->render.skinning.NodeCount();
        segments += mi->render.ribbons.GetTotalSegmentCount();
    }
    particles += particleService_.TotalParticleCount();
}

void RenderService::UploadStagedTextures(Actor& mi) {
    if (!mi.render.textures) mi.render.textures = textures_->CreateModelScope();
    for (auto& [id, st] : mi.render.stagedTextures) {

        if (!st.sharedKey.empty() && st.pixels.empty()) {

            if (mi.render.textures->BindShared(id, st.sharedKey, st.wrapFlags)
                == gfx::TextureHandle::Invalid) {
                std::string msg = "[WDEX texture] eviction race for '";
                msg += st.sharedKey;
                msg += "' — using fallback\n";
                OutputDebugStringA(msg.c_str());
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

void RenderService::uploadTemplateGpu(ModelTemplate& tmpl) {
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
        sg.unskinnedVb = gfx_->CreateBuffer({
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
            sg.unskinnedVb1 = gfx_->CreateBuffer({
                .size  = (u32)(sizeof(Vertex) * sg.vertexCount),
                .usage = gfx::BufferUsage::Vertex,
            }, verticesUv1.data());
        }

        sg.ib = gfx_->CreateBuffer({
            .size  = (u32)(sizeof(u32) * sg.indexCount),
            .usage = gfx::BufferUsage::Index,
        }, mesh.indices.data());

        if ((i32)mesh.tangents.size() == sg.vertexCount) {
            sg.tangentVb = gfx_->CreateBuffer({
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
            sg.boneVb = gfx_->CreateBuffer({
                .size  = (u32)(sizeof(BoneVertex) * sg.vertexCount),
                .usage = gfx::BufferUsage::Vertex,
            }, bv.data());
        }

        tmpl.sharedGeosets.push_back(sg);
    }

    if (!tmpl.templateTextures) tmpl.templateTextures = textures_->CreateModelScope();
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

void RenderService::UploadStagedGeosets(Actor& mi) {
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

            gg.unskinnedVb = gfx_->CreateBuffer({
                .size  = vbBytes,
                .usage = gfx::BufferUsage::Vertex,
            }, sg.vertices.data());

            gg.ib = gfx_->CreateBuffer({
                .size  = (u32)(sizeof(u32) * sg.indices.size()),
                .usage = gfx::BufferUsage::Index,
            }, sg.indices.data());

            if ((i32)sg.tangents.size() == gg.vertexCount) {
                gg.tangentVb = gfx_->CreateBuffer({
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
                gg.boneVb = gfx_->CreateBuffer({
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

void RenderService::CreateNodePalette(Actor& mi) {

    for (auto& geo : mi.render.gpuGeosets) {
        if (geo.boneVb == gfx::BufferHandle::Invalid) continue;
        if (geo.bonePaletteCb != gfx::BufferHandle::Invalid) continue;
        if (mi.render.skinning.GeosetPaletteSize(geo.geosetId) <= 0) continue;
        geo.bonePaletteCb = gfx_->CreateBuffer({
            .size  = sizeof(bls::BonePaletteCb),
            .usage = gfx::BufferUsage::Constant | gfx::BufferUsage::CpuWritable,
        });
        geo.hasSkinning = true;
    }
}

void RenderService::ProcessStagedData() {
    std::lock_guard<std::mutex> lock(dataMutex_);

    for (auto it = scene_->Actors().All().begin(); it != scene_->Actors().All().end(); ) {
        if (it->second->render.stagedClear) {
            const u32 clearedHandle = it->first;
            if (replaceables_) replaceables_->UnregisterModel(*it->second);
            it->second->ReleaseGPU(*gfx_);
            it = scene_->Actors().All().erase(it);
            particleService_.RemoveModel(clearedHandle);
        } else {
            ++it;
        }
    }

    for (auto& [h, miPtr] : scene_->Actors().All()) {
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

void RenderService::ReleaseModelGPU() {
    for (auto& [h, miPtr] : scene_->Actors().All())
        miPtr->ReleaseGPU(*gfx_);
}

void RenderService::UpdateAnimation() {

    std::lock_guard<std::mutex> lock(dataMutex_);

    for (auto& [h, miPtr] : scene_->Actors().All()) {
        auto* mi = miPtr.get();
        if (!mi->render.skinning.HasSkeleton() || !mi->render.skinning.IsReady()) continue;
        if (mi->parentVisibility <= 0.02f) continue;

        mi->render.skinning.ComputeOffsetMatrices();

        for (auto& geo : mi->render.gpuGeosets) {
            if (geo.bonePaletteCb == gfx::BufferHandle::Invalid) continue;
            if (auto bp = bls::ScopedCb<bls::BonePaletteCb>(gfx_.get(), geo.bonePaletteCb)) {

                constexpr i32 kSlots = bls::kMaxBones;
                static thread_local Matrix44f staging[kSlots];
                mi->render.skinning.ComputeGeosetPalette(geo.geosetId, staging, kSlots);
                bls::BuildBonePalette(*bp, staging, kSlots);
            }
        }
    }
}

void RenderService::UpdateParticles(f32 dt) {

    particleService_.Simulate(dt);

    splatService_.Tick();
}

bool RenderService::RenderParticlesBls() {

    if (!blsSdProgram_ || !blsPsoBuilder_) return false;

    auto* cmd = gfx_->GetImmediateContext();

    std::vector<Vertex> verts;
    std::vector<particle::EmitterDrawList> drawLists;
    Matrix44f viewMat;
    {
        std::lock_guard<std::mutex> lock(dataMutex_);
        if (particleService_.EmitterCount() == 0) return true;
        viewMat = scene_->Camera().GetViewMatrix();
        particleService_.BuildGeometry(viewMat, verts, drawLists);
    }
    if (verts.empty()) return true;

    std::stable_sort(drawLists.begin(), drawLists.end(),
        [](const particle::EmitterDrawList& a,
           const particle::EmitterDrawList& b) {
            if (a.priorityPlane != b.priorityPlane) return a.priorityPlane < b.priorityPlane;
            if (a.model         != b.model)         return a.model         < b.model;
            return a.emitterId < b.emitterId;
        });

    const i32 vertCount= (i32)verts.size();

    if (particleServiceVB_ == gfx::BufferHandle::Invalid || vertCount > particleServiceVBSize_) {
        gfx_->Destroy(particleServiceVB_);
        i32 newSize = (std::max)(vertCount, 4096);
        gfx::BufferDesc bd;
        bd.size  = (u32)(sizeof(Vertex) * newSize);
        bd.usage = gfx::BufferUsage::Vertex | gfx::BufferUsage::CpuWritable;
        particleServiceVB_     = gfx_->CreateBuffer(bd);
        particleServiceVBSize_ = newSize;
    }
    if (void* mapped = gfx_->MapBuffer(particleServiceVB_)) {
        memcpy(mapped, verts.data(), sizeof(Vertex) * vertCount);
        gfx_->UnmapBuffer(particleServiceVB_);
    }

    cmd->BindVertexBuffer(0, particleServiceVB_, sizeof(Vertex));

    bls::FrameInputs frame;
    frame.world      = Matrix44f::identity();
    frame.view       = viewMat;
    const f32 aspect= (height_ > 0) ? (f32)width_ / (f32)height_ : 1.0f;
    frame.projection = scene_->Camera().ProjectionRH(aspect);
    frame.effectTime = scene_->GetAnimationTime() * 0.001f;
    frame.numLights  = 0;
    frame.viewportRect = { (f32)width_, (f32)height_, 0.0f, 0.0f };

    for (const auto& dl : drawLists) {
        if (dl.vertexCount <= 0) continue;

        bls::MatParams mp = bls::FromParticleDesc(dl.material, bls::GxShaderID::SD);
        mp.disables |= bls::kDisableLighting;

        mp.diffuseColor = {1, 1, 1, 1};

        bls::RenderState rs;
        rs.shaderId       = bls::GxShaderID::SD;
        rs.alphaMode      = static_cast<u8>(mp.alpha);
        rs.numColors      = 1;
        rs.numTexCoords   = 1;
        rs.numWeights     = 0;
        rs.numLights      = 0;
        rs.fogEnabled     = false;
        rs.depthWrite     = mp.DepthWriteEnabled();
        rs.lightingEnabled= false;
        auto perm = bls::SelectPermutes(rs);

        auto req = bls::MakePsoRequest(blsSdProgram_,
                                       bls::VertexLayoutKind::ParticleSD,
                                       mp, perm);
        req.rtvFormat = SceneTargetFormat();
        auto pso = blsPsoBuilder_->GetOrBuild(req);
        if (pso == gfx::PipelineHandle::Invalid) continue;
        cmd->BindPipeline(pso);

        if (auto vs = bls::ScopedCb<bls::SdVsCbA>(gfx_.get(), blsSdVsCb_)) {
            bls::BuildSdVsCbA(*vs, frame, mp);
        }
        if (auto ps = bls::ScopedCb<bls::SdPsCbA>(gfx_.get(), blsSdPsCb_)) {
            bls::BuildSdPsCbA(*ps, frame, mp);
        }
        cmd->BindConstantBuffer(gfx::ShaderStage::Vertex, 0, blsSdVsCb_);
        cmd->BindConstantBuffer(gfx::ShaderStage::Pixel,  0, blsSdPsCb_);

        const u32 wrapFlags = 0x3;

        gfx::TextureHandle peTex = gfx::TextureHandle::Invalid;
        if (dl.material.replaceableId == 1 && replaceables_) {
            peTex = replaceables_->GetSdTeamColorTexture();
        } else if (dl.material.replaceableId == 2 && replaceables_) {
            peTex = replaceables_->GetSdTeamGlowTexture();
        } else {
            std::lock_guard<std::mutex> lock(dataMutex_);
            Actor* owner = getModel(dl.model);
            if (owner && owner->render.textures && dl.material.textureId >= 0)
                peTex = owner->render.textures->Get(dl.material.textureId);
        }
        if (peTex != gfx::TextureHandle::Invalid)
            cmd->BindShaderResource(gfx::ShaderStage::Pixel, 0, peTex);
        else
            cmd->BindShaderResource(gfx::ShaderStage::Pixel, 0, textures_->GetDefaults().White);
        cmd->BindSampler(gfx::ShaderStage::Pixel, 0, samplers_->WrapVariant(wrapFlags));

        cmd->Draw(dl.vertexCount, dl.vertexOffset);
    }
    return true;
}

namespace {
particle::FilterMode SplatBlendModeToFilter(i32 blendMode) {

    switch (blendMode) {
        case 0: return particle::FilterMode::Blend;
        case 1: return particle::FilterMode::Additive;
        case 2: return particle::FilterMode::Modulate;
        case 3: return particle::FilterMode::Modulate2X;
        case 4: return particle::FilterMode::AlphaKey;
        default: return particle::FilterMode::Blend;
    }
}
}

bool RenderService::RenderSplatsBls() {
    if (!blsSdProgram_ || !blsPsoBuilder_) return false;
    if (splatService_.Count() == 0)        return true;

    auto* cmd = gfx_->GetImmediateContext();

    std::vector<Vertex>                       verts;
    std::vector<particle::SplatDrawList>      drawLists;
    Matrix44f viewMat = scene_->Camera().GetViewMatrix();
    splatService_.BuildGeometry(verts, drawLists);
    if (verts.empty()) return true;

    const i32 vertCount= (i32)verts.size();
    if (splatServiceVB_ == gfx::BufferHandle::Invalid || vertCount > splatServiceVBSize_) {
        gfx_->Destroy(splatServiceVB_);
        i32 newSize = (std::max)(vertCount, 4096);
        gfx::BufferDesc bd;
        bd.size  = (u32)(sizeof(Vertex) * newSize);
        bd.usage = gfx::BufferUsage::Vertex | gfx::BufferUsage::CpuWritable;
        splatServiceVB_     = gfx_->CreateBuffer(bd);
        splatServiceVBSize_ = newSize;
    }
    if (void* mapped = gfx_->MapBuffer(splatServiceVB_)) {
        memcpy(mapped, verts.data(), sizeof(Vertex) * vertCount);
        gfx_->UnmapBuffer(splatServiceVB_);
    }

    cmd->BindVertexBuffer(0, splatServiceVB_, sizeof(Vertex));

    bls::FrameInputs frame;
    frame.world      = Matrix44f::identity();
    frame.view       = viewMat;
    const f32 aspect= (height_ > 0) ? (f32)width_ / (f32)height_ : 1.0f;
    frame.projection = scene_->Camera().ProjectionRH(aspect);
    frame.effectTime = scene_->GetAnimationTime() * 0.001f;
    frame.numLights  = 0;
    frame.viewportRect = { (f32)width_, (f32)height_, 0.0f, 0.0f };

    for (const auto& dl : drawLists) {
        if (dl.vertexCount <= 0) continue;

        particle::ParticleMaterialDesc pmd;
        pmd.filterMode = SplatBlendModeToFilter(dl.blendMode);
        pmd.unshaded   = true;
        pmd.unfogged   = true;
        pmd.textureId  = -1;

        bls::MatParams mp = bls::FromParticleDesc(pmd, bls::GxShaderID::SD);
        mp.disables |= bls::kDisableLighting;
        mp.diffuseColor = {1, 1, 1, 1};

        bls::RenderState rs;
        rs.shaderId        = bls::GxShaderID::SD;
        rs.alphaMode       = static_cast<u8>(mp.alpha);
        rs.numColors       = 1;
        rs.numTexCoords    = 1;
        rs.numWeights      = 0;
        rs.numLights       = 0;
        rs.fogEnabled      = false;
        rs.depthWrite      = mp.DepthWriteEnabled();
        rs.lightingEnabled = false;
        auto perm = bls::SelectPermutes(rs);

        auto req = bls::MakePsoRequest(blsSdProgram_,
                                       bls::VertexLayoutKind::ParticleSD,
                                       mp, perm);
        req.rtvFormat = SceneTargetFormat();
        auto pso = blsPsoBuilder_->GetOrBuild(req);
        if (pso == gfx::PipelineHandle::Invalid) continue;
        cmd->BindPipeline(pso);

        if (auto vs = bls::ScopedCb<bls::SdVsCbA>(gfx_.get(), blsSdVsCb_)) {
            bls::BuildSdVsCbA(*vs, frame, mp);
        }
        if (auto ps = bls::ScopedCb<bls::SdPsCbA>(gfx_.get(), blsSdPsCb_)) {
            bls::BuildSdPsCbA(*ps, frame, mp);
        }
        cmd->BindConstantBuffer(gfx::ShaderStage::Vertex, 0, blsSdVsCb_);
        cmd->BindConstantBuffer(gfx::ShaderStage::Pixel,  0, blsSdPsCb_);

        const u32 wrapFlags = 0x3;
        if (dl.texture != gfx::TextureHandle::Invalid)
            cmd->BindShaderResource(gfx::ShaderStage::Pixel, 0, dl.texture);
        else
            cmd->BindShaderResource(gfx::ShaderStage::Pixel, 0, textures_->GetDefaults().White);
        cmd->BindSampler(gfx::ShaderStage::Pixel, 0, samplers_->WrapVariant(wrapFlags));

        cmd->Draw(dl.vertexCount, dl.vertexOffset);
    }
    return true;
}

void RenderService::UpdateRibbons(f32 dt) {
    std::lock_guard<std::mutex> lock(dataMutex_);
    for (auto& [h, mi] : scene_->Actors().All()) {
        if (mi->parentVisibility <= 0.02f) continue;
        mi->render.ribbons.Simulate(dt);
    }
}

void RenderService::RenderRibbons() {
    if (!blsSdProgram_ || !blsPsoBuilder_) return;
    auto* cmd = gfx_->GetImmediateContext();

    bls::FrameInputs frame;
    frame.world        = Matrix44f::identity();
    const f32 aspect= (height_ > 0) ? (f32)width_ / (f32)height_ : 1.0f;
    frame.numLights    = 0;
    frame.viewportRect = { (f32)width_, (f32)height_, 0.0f, 0.0f };
    frame.effectTime   = scene_->GetAnimationTime() * 0.001f;

    for (auto& [_mh, _mi] : scene_->Actors().All()) {
    auto* mi = _mi.get();
    Matrix44f viewMat;
    RibbonSystem::StripResult stripResult;
    std::vector<RibbonEmitterConfig> configs;

    {
        std::lock_guard<std::mutex> lock(dataMutex_);
        if (!mi->render.ribbons.HasEmitters()) continue;
        if (mi->parentVisibility <= 0.02f) continue;
        viewMat = scene_->Camera().GetViewMatrix();
        stripResult = mi->render.ribbons.BuildStrips();
        for (i32 eid: stripResult.emitterIds) {
            auto* c = mi->render.ribbons.GetConfig(eid);
            configs.push_back(c ? *c : RibbonEmitterConfig{});
        }
    }

    auto& verts = stripResult.vertices;
    auto& emitterIds = stripResult.emitterIds;
    if (verts.empty()) continue;
    i32 vertCount = (i32)verts.size();

    if (mi->render.ribbonVB == gfx::BufferHandle::Invalid || vertCount > mi->render.ribbonVBSize) {
        gfx_->Destroy(mi->render.ribbonVB);
        i32 newSize = (std::max)(vertCount, 512);
        gfx::BufferDesc bd;
        bd.size  = (u32)(sizeof(Vertex) * newSize);
        bd.usage = gfx::BufferUsage::Vertex | gfx::BufferUsage::CpuWritable;
        mi->render.ribbonVB = gfx_->CreateBuffer(bd);
        mi->render.ribbonVBSize = newSize;
    }

    void* mapped = gfx_->MapBuffer(mi->render.ribbonVB);
    if (!mapped) continue;
    memcpy(mapped, verts.data(), sizeof(Vertex) * vertCount);
    gfx_->UnmapBuffer(mi->render.ribbonVB);

    cmd->BindVertexBuffer(0, mi->render.ribbonVB, sizeof(Vertex));

    frame.view       = viewMat;
    frame.projection = scene_->Camera().ProjectionRH(aspect);

    std::vector<i32> vertCounts;
    std::vector<i32> vertOffsets;
    {
        std::lock_guard<std::mutex> lock(dataMutex_);
        i32 running = 0;
        for (i32 eid: emitterIds) {
            vertOffsets.push_back(running);
            const i32 n = mi->render.ribbons.GetEmitterVertCount(eid);
            vertCounts.push_back(n);
            running += n;
        }
    }

    std::vector<i32> drawOrder(emitterIds.size());
    for (i32 i= 0; i < (i32)drawOrder.size(); ++i) drawOrder[i] = i;
    std::stable_sort(drawOrder.begin(), drawOrder.end(),
        [&](i32 a, i32 b) {
            const i32 pa = configs[a].priorityPlane;
            const i32 pb = configs[b].priorityPlane;
            if (pa != pb) return pa < pb;
            return emitterIds[a] < emitterIds[b];
        });

    for (i32 ei: drawOrder) {
        auto& cfg = configs[ei];
        const i32 count   = vertCounts[ei];
        const i32 offset  = vertOffsets[ei];
        if (count <= 0) continue;

        i32 matFlags = 0;
        if (cfg.twoSided) matFlags |= MAT_TWO_SIDED;
        if (cfg.unshaded) matFlags |= MAT_UNSHADED;
        bls::MatParams mp = bls::FromMdxLayer(cfg.filterMode, matFlags, bls::GxShaderID::SD);
        mp.disables |= bls::kDisableLighting;
        mp.diffuseColor = {1, 1, 1, 1};

        bls::RenderState rs = bls::MakeSdMeshRenderState(mp, 0,  true,  false);
        auto perm = bls::SelectPermutes(rs);
        auto req = bls::MakePsoRequest(blsSdProgram_,
                                       bls::VertexLayoutKind::ParticleSD,
                                       mp, perm);
        req.rtvFormat = SceneTargetFormat();
        auto pso = blsPsoBuilder_->GetOrBuild(req);
        if (pso == gfx::PipelineHandle::Invalid) continue;
        cmd->BindPipeline(pso);

        if (auto vs = bls::ScopedCb<bls::SdVsCbA>(gfx_.get(), blsSdVsCb_)) {
            bls::BuildSdVsCbA(*vs, frame, mp);
        }
        if (auto ps = bls::ScopedCb<bls::SdPsCbA>(gfx_.get(), blsSdPsCb_)) {
            bls::BuildSdPsCbA(*ps, frame, mp);
        }
        cmd->BindConstantBuffer(gfx::ShaderStage::Vertex, 0, blsSdVsCb_);
        cmd->BindConstantBuffer(gfx::ShaderStage::Pixel,  0, blsSdPsCb_);

        render_detail::BindLayerAlbedo(cmd, mi->render.textures.get(), cfg.textureId,
                                       textures_->GetDefaults().White, *samplers_);

        cmd->Draw(count, offset);
    }
    }
}

i32 RenderService::ComputeSelectedLod() const {
    i32 ov = lodOverride_.load();
    if (ov >= 0) return std::clamp(ov, 0, 3);

    Vector3f camPos = scene_->Camera().GetSource();
    f32 viewDist = std::sqrt(camPos.x*camPos.x + camPos.y*camPos.y + camPos.z*camPos.z);
    if (viewDist < 1.0f) return 0;

    const f32 aspect= (height_ > 0) ? (f32)width_ / (f32)height_ : 1.0f;
    Matrix44f proj = scene_->Camera().ProjectionLH(aspect);

    f32 projM11 = proj.data[1][1];
    f32 screenPixels = projM11 / viewDist * (f32)height_ * 0.5f;
    if (screenPixels <= 0.001f) return 3;
    f32 deviation = 5.0f / screenPixels;

    static constexpr f32 kDeviations[4] = {0.0f, 1.0f, 2.0f, 4.0f};
    i32 selectedLOD = 4;
    do { --selectedLOD; }
    while (selectedLOD > 0 && deviation <= kDeviations[selectedLOD]);
    return std::clamp(selectedLOD, 0, 3);
}

bool RenderService::InitDevice(gfx::GfxApi api) {

    gfx_ = gfx::CreateDevice(api);
    if (!gfx_) return false;

    samplers_ = std::make_unique<SamplerAssetManager>(*gfx_);

    cbPerFrame_ = gfx_->CreateBuffer({
        .size  = sizeof(CBPerFrame),
        .usage = gfx::BufferUsage::Constant | gfx::BufferUsage::CpuWritable,
    });

    textures_     = std::make_unique<TextureAssetManager>(*gfx_);

    replaceables_ = std::make_unique<ReplaceableTextureManager>(*gfx_, *textures_);

    if (!CreateShaders())          { CleanupD3D(); return false; }
    if (!CreatePipelines())        { CleanupD3D(); return false; }
    if (!CreateDefaultResources()) { CleanupD3D(); return false; }

    if (!InitBlsShaders()) { CleanupD3D(); return false; }

    return true;
}

bool RenderService::InitBlsShaders() {
    if (!gfx_ || !scene_->ActiveContentProvider()) return false;

    if (replaceables_) replaceables_->SetContentProvider(scene_->ActiveContentProvider());

    splatService_.Configure(gfx_.get(), textures_.get(),
                            scene_->ActiveContentProvider());

    if (!dncService_) {
        dncService_ = std::make_unique<dnc::DncService>(scene_->ActiveContentProvider());
    }

    if (!shadowService_) {
        shadowService_ = std::make_unique<shadow::ShadowService>(gfx_.get());
    }

    if (shadowVsCb_ == gfx::BufferHandle::Invalid) {
        shadowVsCb_ = gfx_->CreateBuffer({
            .size  = sizeof(bls::HdVsCb),
            .usage = gfx::BufferUsage::Constant | gfx::BufferUsage::CpuWritable,
        });
    }

    blsShaderCache_ = std::make_unique<bls::BlsShaderCache>(gfx_.get(), scene_->ActiveContentProvider());
    blsPrograms_    = std::make_unique<bls::BlsProgramCatalog>(blsShaderCache_.get());
    blsPsoBuilder_  = std::make_unique<bls::BlsPsoBuilder>(gfx_.get());

    blsSdProgram_ = blsPrograms_->Load({
        bls::GxShaderID::SD, "SD_HighSpec", "SD"
    });
    blsSdOnHdProgram_ = blsPrograms_->Load({
        bls::GxShaderID::SD_on_HD, "SD_on_HD", "SD_on_HD"
    });
    blsHdProgram_ = blsPrograms_->Load({
        bls::GxShaderID::HD, "HD", "HD"
    });

    blsCrystalProgram_ = blsPrograms_->Load({
        bls::GxShaderID::Crystal, "HD", "Crystal"
    });

    if (blsHdProgram_ && blsHdProgram_->vs &&
        !blsHdProgram_->vs->permuteHandles.empty()) {
        const shadow::ShadowParams& sp = shadowService_
            ? shadowService_->Params()
            : shadow::ShadowParams{};

        auto buildShadowPso = [&](u32 permIndex,
                                   bls::VertexLayoutKind layoutKind)
            -> gfx::PipelineHandle {
            if (permIndex >= blsHdProgram_->vs->permuteHandles.size())
                return gfx::PipelineHandle::Invalid;
            gfx::GraphicsPipelineDesc gpd{};
            gpd.vs                              = blsHdProgram_->vs->permuteHandles[permIndex];
            gpd.ps                              = gfx::ShaderHandle{0};
            gpd.inputLayout                     = bls::LayoutFor(layoutKind);
            gpd.topology                        = gfx::PrimitiveTopology::TriangleList;
            gpd.depthStencil.depthTest          = true;
            gpd.depthStencil.depthWrite         = true;
            gpd.depthStencil.depthCompare       = gfx::CompareOp::LessEqual;
            gpd.rasterizer.cull                 = gfx::CullMode::Back;
            gpd.rasterizer.frontCCW             = true;
            gpd.rasterizer.depthBias            = sp.depthBias;
            gpd.rasterizer.slopeScaledDepthBias = sp.slopeScaledBias;
            gpd.rasterizer.depthBiasClamp       = sp.depthBiasClamp;
            gpd.rtvFormat                       = gfx::Format::Unknown;
            gpd.dsvFormat                       = gfx::Format::D32_FLOAT;
            return gfx_->CreateGraphicsPipeline(gpd);
        };

        if (shadowPSO_ == gfx::PipelineHandle::Invalid) {
            shadowPSO_ = buildShadowPso(
                 4,
                bls::VertexLayoutKind::MeshHDSkinnedNoTangent);
        }
        if (shadowPSORigid_ == gfx::PipelineHandle::Invalid) {
            shadowPSORigid_ = buildShadowPso(
                 0,
                bls::VertexLayoutKind::ParticleSD);
        }
    }

    blsSpriteVs_  = blsShaderCache_->Acquire(gfx::ShaderStage::Vertex, "sprite");
    blsTonemapPs_ = blsShaderCache_->Acquire(gfx::ShaderStage::Pixel,  "tonemap");

    if (blsSpriteVs_ && !blsSpriteVs_->permuteHandles.empty()
        && blsTonemapPs_ && !blsTonemapPs_->permuteHandles.empty())
    {
        struct TonemapVertex {
            f32 x, y, z;
            f32 u, v;
        };

        static const TonemapVertex kTonemapVerts[3] = {

            { -1.0f,  1.0f, 0.0f,         0.0f, 0.0f },
            {  3.0f,  1.0f, 0.0f,         2.0f, 0.0f },
            { -1.0f, -3.0f, 0.0f,         0.0f, 2.0f },
        };
        tonemapVB_ = gfx_->CreateBuffer({
            .size  = sizeof(kTonemapVerts),
            .usage = gfx::BufferUsage::Vertex,
        }, kTonemapVerts);

        const gfx::InputElement spriteInput[] = {
            {"ATTR", 0, gfx::Format::R32G32B32_FLOAT, 0},
            {"ATTR", 3, gfx::Format::R32G32_FLOAT,    12},
        };
        gfx::GraphicsPipelineDesc tm;
        tm.vs              = blsSpriteVs_->permuteHandles[0];
        tm.ps              = blsTonemapPs_->permuteHandles[0];
        tm.inputLayout     = spriteInput;
        tm.topology        = gfx::PrimitiveTopology::TriangleList;
        tm.blend.enable    = false;
        tm.depthStencil.depthTest  = false;
        tm.depthStencil.depthWrite = false;
        tm.rasterizer.cull     = gfx::CullMode::None;
        tm.rasterizer.frontCCW = true;
        tm.rtvFormat       = gfx::Format::R8G8B8A8_UNORM_SRGB;
        tm.dsvFormat       = gfx::Format::D24_UNORM_S8_UINT;
        tonemapPSO_ = gfx_->CreateGraphicsPipeline(tm);

        tonemapPsCb_ = gfx_->CreateBuffer({
            .size  = 16,
            .usage = gfx::BufferUsage::Constant | gfx::BufferUsage::CpuWritable,
        });
        gfx::SamplerDesc sd;
        sd.minFilter = gfx::Filter::Linear;
        sd.magFilter = gfx::Filter::Linear;
        sd.addressU  = gfx::AddressMode::Clamp;
        sd.addressV  = gfx::AddressMode::Clamp;
        sd.addressW  = gfx::AddressMode::Clamp;
        tonemapSampler_ = gfx_->CreateSampler(sd);
    }

    blsSdVsCb_ = gfx_->CreateBuffer({
        .size  = sizeof(bls::SdVsCbA),
        .usage = gfx::BufferUsage::Constant | gfx::BufferUsage::CpuWritable,
    });

    blsSdPsCb_ = gfx_->CreateBuffer({
        .size  = sizeof(bls::SdPsCbA),
        .usage = gfx::BufferUsage::Constant | gfx::BufferUsage::CpuWritable,
    });
    blsHdVsCb_ = gfx_->CreateBuffer({
        .size  = sizeof(bls::HdVsCb),
        .usage = gfx::BufferUsage::Constant | gfx::BufferUsage::CpuWritable,
    });

    blsHdShadowCb_ = gfx_->CreateBuffer({
        .size  = sizeof(bls::HdShadowCascadesCb),
        .usage = gfx::BufferUsage::Constant | gfx::BufferUsage::CpuWritable,
    });

    blsHdShadowCountCb_ = gfx_->CreateBuffer({
        .size  = sizeof(bls::SdOnHdShadowCascadeCountCb),
        .usage = gfx::BufferUsage::Constant | gfx::BufferUsage::CpuWritable,
    });
    blsHdPsCb_ = gfx_->CreateBuffer({
        .size  = sizeof(bls::HdPsCb),
        .usage = gfx::BufferUsage::Constant | gfx::BufferUsage::CpuWritable,
    });
    blsSdOnHdPsCb_ = gfx_->CreateBuffer({
        .size  = sizeof(bls::SdOnHdPsCb),
        .usage = gfx::BufferUsage::Constant | gfx::BufferUsage::CpuWritable,
    });
    blsHdDebugVisCb_ = gfx_->CreateBuffer({
        .size  = sizeof(bls::DebugVisCb),
        .usage = gfx::BufferUsage::Constant | gfx::BufferUsage::CpuWritable,
    });

    textures_->RegisterOwned(kIblSplitSumLutName, ibl::CreateSplitSumLutTexture(*gfx_));

    ApplyIblMode(iblMode_);

    return blsSdProgram_     != nullptr
        && blsSdOnHdProgram_ != nullptr
        && blsHdProgram_     != nullptr
        && blsSpriteVs_      != nullptr
        && blsTonemapPs_     != nullptr
        && tonemapPSO_       != gfx::PipelineHandle::Invalid;
}

void RenderService::SetEnvProbe(const std::string& relPath) {
    if (!gfx_ || !textures_) return;

    textures_->ReleaseOwned(kIblFromProbeName);
    textures_->ReleaseOwned(kIblToProbeName);

    gfx::TextureHandle fromHandle = gfx::TextureHandle::Invalid;
    i32 mips = 0;
    if (!relPath.empty() && scene_->ActiveContentProvider()) {
        auto probe = ibl::LoadEnvProbe(*gfx_, *scene_->ActiveContentProvider(), relPath);
        if (probe.handle != gfx::TextureHandle::Invalid) {
            fromHandle = probe.handle;
            mips       = probe.mipCount;
        }
    }
    if (fromHandle == gfx::TextureHandle::Invalid) {
        std::fprintf(stderr,
                     "[ibl] WARN: probe '%s' failed to load — using debug "
                     "procedural\n",
                     relPath.c_str());
        fromHandle = ibl::CreateDebugFacesEnvProbe(*gfx_);
        mips       = ibl::kEnvProbeMipLevels;
    }
    textures_->RegisterOwned(kIblFromProbeName, fromHandle);
    iblProbeMipEnd_ = static_cast<f32>(mips - 1);

    iblDayNightLoaded_ = false;
    textures_->ReleaseOwned(kIblDayProbeName);
    textures_->ReleaseOwned(kIblNightProbeName);
}

void RenderService::SetDayNightProbes(const std::string& dayPath,
                                      const std::string& nightPath) {
    if (!gfx_ || !textures_ || !scene_->ActiveContentProvider()) return;

    textures_->ReleaseOwned(kIblDayProbeName);
    textures_->ReleaseOwned(kIblNightProbeName);
    iblDayNightLoaded_ = false;
    iblDayMipEnd_      = 0.0f;
    iblNightMipEnd_    = 0.0f;

    auto loadProbe = [&](const std::string& path,
                         const char* slotName) -> i32 {
        if (path.empty()) return 0;
        auto probe = ibl::LoadEnvProbe(*gfx_, *scene_->ActiveContentProvider(), path);
        if (probe.handle == gfx::TextureHandle::Invalid) {
            std::fprintf(stderr, "[dnc] day/night IBL load failed: %s\n", path.c_str());
            return 0;
        }
        textures_->RegisterOwned(slotName, probe.handle);
        return probe.mipCount;
    };

    const i32 dayMips   = loadProbe(dayPath,   kIblDayProbeName);
    const i32 nightMips = loadProbe(nightPath, kIblNightProbeName);
    if (dayMips > 0 && nightMips > 0) {
        iblDayMipEnd_      = static_cast<f32>(dayMips - 1);
        iblNightMipEnd_    = static_cast<f32>(nightMips - 1);
        iblDayNightLoaded_ = true;
    } else {

        textures_->ReleaseOwned(kIblDayProbeName);
        textures_->ReleaseOwned(kIblNightProbeName);
    }
}

void RenderService::SetIblMode(IblMode mode) {
    iblMode_ = mode;

    ApplyIblMode(mode);
}

void RenderService::ApplyIblMode(IblMode mode) {
    switch (mode) {
        case IblMode::DayNight:
            SetDayNightProbes(ibl::kDayIblPath, ibl::kNightIblPath);

            if (!iblDayNightLoaded_) SetEnvProbe(ibl::kPortraitIblPath);
            return;
        case IblMode::Dungeon:
            SetEnvProbe(ibl::kDungeonIblPath);
            return;
        case IblMode::Sunset:
            SetEnvProbe(ibl::kSunsetIblPath);
            return;
        case IblMode::Portrait:
            break;
    }
    SetEnvProbe(ibl::kPortraitIblPath);
}

void RenderService::ShutdownBlsShaders() {
    blsSdProgram_      = nullptr;
    blsSdOnHdProgram_  = nullptr;
    blsHdProgram_      = nullptr;
    blsCrystalProgram_ = nullptr;

    blsSpriteVs_      = nullptr;
    blsTonemapPs_     = nullptr;
    if (gfx_) {
        gfx_->Destroy(blsSdVsCb_);     blsSdVsCb_     = gfx::BufferHandle::Invalid;
        gfx_->Destroy(blsSdPsCb_);     blsSdPsCb_     = gfx::BufferHandle::Invalid;
        gfx_->Destroy(blsHdVsCb_);     blsHdVsCb_     = gfx::BufferHandle::Invalid;
        gfx_->Destroy(blsHdShadowCb_); blsHdShadowCb_ = gfx::BufferHandle::Invalid;
        gfx_->Destroy(blsHdShadowCountCb_); blsHdShadowCountCb_ = gfx::BufferHandle::Invalid;
        if (shadowPSO_  != gfx::PipelineHandle::Invalid) {
            gfx_->Destroy(shadowPSO_);  shadowPSO_  = gfx::PipelineHandle::Invalid;
        }
        if (shadowPSORigid_ != gfx::PipelineHandle::Invalid) {
            gfx_->Destroy(shadowPSORigid_);
            shadowPSORigid_ = gfx::PipelineHandle::Invalid;
        }
        if (shadowVsCb_ != gfx::BufferHandle::Invalid) {
            gfx_->Destroy(shadowVsCb_); shadowVsCb_ = gfx::BufferHandle::Invalid;
        }
        gfx_->Destroy(blsHdPsCb_);     blsHdPsCb_     = gfx::BufferHandle::Invalid;
        gfx_->Destroy(blsSdOnHdPsCb_); blsSdOnHdPsCb_ = gfx::BufferHandle::Invalid;
        gfx_->Destroy(blsHdDebugVisCb_); blsHdDebugVisCb_ = gfx::BufferHandle::Invalid;

        if (textures_) {
            textures_->ReleaseOwned(kIblFromProbeName);
            textures_->ReleaseOwned(kIblToProbeName);
            textures_->ReleaseOwned(kIblDayProbeName);
            textures_->ReleaseOwned(kIblNightProbeName);
            textures_->ReleaseOwned(kIblSplitSumLutName);
        }
        iblDayNightLoaded_ = false;
    }
    if (blsPsoBuilder_)  blsPsoBuilder_->Clear();
    if (blsPrograms_)    blsPrograms_->Clear();
    if (blsShaderCache_) blsShaderCache_->ReleaseAll();
    blsPsoBuilder_.reset();
    blsPrograms_.reset();
    blsShaderCache_.reset();
}

RenderTargetId RenderService::CreateSwapChainTarget(void* nativeWindowHandle, i32 w, i32 h) {
    if (!gfx_) return 0;

    RenderTarget target;
    target.id   = nextTargetId_++;
    target.swap  = gfx_->CreateSwapChain(nativeWindowHandle, w, h);
    if (target.swap == gfx::SwapChainHandle::Invalid) return 0;

    target.color       = gfx_->GetSwapChainBackBuffer(target.swap);
    target.colorLinear = gfx_->GetSwapChainBackBufferLinear(target.swap);
    target.hdrColor    = gfx_->CreateColorTarget(w, h, kHdrSceneFormat);
    target.depth       = gfx_->CreateDepthTarget(w, h, gfx::Format::D24_UNORM_S8_UINT);
    target.width       = w;
    target.height      = h;

    RenderTargetId id = target.id;
    targets_[id] = target;
    return id;
}

RenderTargetId RenderService::CreateOffscreenTarget(i32 w, i32 h) {
    if (!gfx_) return 0;

    RenderTarget target;
    target.id          = nextTargetId_++;

    target.color       = gfx_->CreateColorTarget(w, h, gfx::Format::R8G8B8A8_UNORM_SRGB);
    target.colorLinear = target.color;
    target.hdrColor    = gfx_->CreateColorTarget(w, h, kHdrSceneFormat);
    target.depth       = gfx_->CreateDepthTarget(w, h, gfx::Format::D24_UNORM_S8_UINT);
    target.width       = w;
    target.height      = h;

    if (target.color == gfx::TextureHandle::Invalid) return 0;

    RenderTargetId id = target.id;
    targets_[id] = target;
    return id;
}

void RenderService::DestroyRenderTarget(RenderTargetId id) {
    auto it = targets_.find(id);
    if (it == targets_.end()) return;
    auto& t = it->second;
    if (t.swap != gfx::SwapChainHandle::Invalid) {
        gfx_->DestroySwapChain(t.swap);
    } else {
        gfx_->Destroy(t.color);
    }
    gfx_->Destroy(t.hdrColor);
    gfx_->Destroy(t.depth);
    targets_.erase(it);
}

void RenderService::ResizeRenderTarget(RenderTargetId id, i32 w, i32 h) {
    auto it = targets_.find(id);
    if (it == targets_.end() || !gfx_) return;
    auto& t = it->second;

    gfx_->Destroy(t.depth);
    gfx_->Destroy(t.hdrColor);

    if (t.swap != gfx::SwapChainHandle::Invalid) {
        gfx_->ResizeSwapChain(t.swap, w, h);
        t.color       = gfx_->GetSwapChainBackBuffer(t.swap);
        t.colorLinear = gfx_->GetSwapChainBackBufferLinear(t.swap);
    } else {
        gfx_->Destroy(t.color);
        t.color       = gfx_->CreateColorTarget(w, h, gfx::Format::R8G8B8A8_UNORM_SRGB);
        t.colorLinear = t.color;
    }

    t.hdrColor = gfx_->CreateColorTarget(w, h, kHdrSceneFormat);
    t.depth    = gfx_->CreateDepthTarget(w, h, gfx::Format::D24_UNORM_S8_UINT);
    t.width    = w;
    t.height   = h;
}

void RenderService::ResizePrimaryTarget(i32 w, i32 h) {
    auto* target = primaryTarget();
    if (!target || !gfx_) return;
    ResizeRenderTarget(target->id, w, h);
    width_ = w;
    height_ = h;
}

void RenderService::CleanupD3D() {

    ShutdownBlsShaders();

    if (gfx_) {

        gfx_->Destroy(lineVS_);  gfx_->Destroy(linePS_);
        gfx_->Destroy(linePSOHdr_);
        gfx_->Destroy(linePSOSd_);
        gfx_->Destroy(tonemapPSO_);
        gfx_->Destroy(tonemapVB_);
        gfx_->Destroy(tonemapPsCb_);
        gfx_->Destroy(tonemapSampler_);
        tonemapPSO_     = gfx::PipelineHandle::Invalid;
        tonemapVB_      = gfx::BufferHandle::Invalid;
        tonemapPsCb_    = gfx::BufferHandle::Invalid;
        tonemapSampler_ = gfx::SamplerHandle::Invalid;

        gfx_->Destroy(cbPerFrame_);

        if (replaceables_) replaceables_->Shutdown();
        replaceables_.reset();
        samplers_.reset();
        textures_.reset();

        debug_->DestroyResources();

        gfx_->Destroy(particleServiceVB_);
        particleServiceVB_ = gfx::BufferHandle::Invalid;
        particleServiceVBSize_ = 0;

        gfx_->Destroy(splatServiceVB_);
        splatServiceVB_ = gfx::BufferHandle::Invalid;
        splatServiceVBSize_ = 0;

        for (auto& [id, t] : targets_) {
            if (t.swap != gfx::SwapChainHandle::Invalid)
                gfx_->DestroySwapChain(t.swap);
            else
                gfx_->Destroy(t.color);
            gfx_->Destroy(t.hdrColor);
            gfx_->Destroy(t.depth);
        }
    }
    targets_.clear();
    primaryTargetId_ = 0;

    gfx_.reset();
}

bool RenderService::CreateShaders() {
    using namespace WhiteoutDex::Shaders;

    lineVS_ = gfx_->CreateShader(gfx::ShaderStage::Vertex, kLineVS, sizeof(kLineVS));
    linePS_ = gfx_->CreateShader(gfx::ShaderStage::Pixel,  kLinePS, sizeof(kLinePS));

    return lineVS_ != gfx::ShaderHandle::Invalid
        && linePS_ != gfx::ShaderHandle::Invalid;
}

bool RenderService::CreatePipelines() {
    using namespace gfx;

    InputElement lineInput[] = {
        {"POSITION", 0, Format::R32G32B32_FLOAT,    0},
        {"COLOR",    0, Format::R32G32B32A32_FLOAT, 12},
    };

    GraphicsPipelineDesc desc;
    desc.vs          = lineVS_;
    desc.ps          = linePS_;
    desc.inputLayout = lineInput;
    desc.topology    = PrimitiveTopology::LineList;
    desc.blend.enable = false;
    desc.depthStencil = {};
    desc.rasterizer.cull     = CullMode::None;
    desc.rasterizer.frontCCW = true;

    desc.rtvFormat = kHdrSceneFormat;
    linePSOHdr_    = gfx_->CreateGraphicsPipeline(desc);

    desc.rtvFormat = kSdSceneFormat;
    linePSOSd_     = gfx_->CreateGraphicsPipeline(desc);

    return linePSOHdr_ != PipelineHandle::Invalid
        && linePSOSd_  != PipelineHandle::Invalid;
}

gfx::PipelineHandle RenderService::CurrentLinePSO() const {
    return renderMode_ == RenderMode::HD ? linePSOHdr_ : linePSOSd_;
}

bool RenderService::CreateDefaultResources() {
    return debug_->CreateResources();
}

void RenderService::RenderFrame(RenderTargetId targetId) {
    auto it = targets_.find(targetId);
    if (it == targets_.end()) return;
    auto& target = it->second;
    if (target.color    == gfx::TextureHandle::Invalid || !gfx_) return;
    if (target.hdrColor == gfx::TextureHandle::Invalid)         return;

    {
        std::lock_guard<std::mutex> lock(dataMutex_);
        const i32 activeIdx = scene_->ActiveCameraPresetIdx();
        const auto& presets = scene_->CameraPresets();
        if (activeIdx >= 0 && activeIdx < (i32)presets.size()) {
            const auto& preset = presets[activeIdx];
            if (preset.animator) {
                i32 seqStart = 0, seqEnd = 0;
                Actor* focus = scene_->FocusActor();
                i32 idx = focus ? focus->animation.ActiveSequenceIndex() : 0;
                const auto& ranges = scene_->SequenceRanges();
                if (idx >= 0 && idx < (i32)ranges.size()) {
                    seqStart = ranges[idx].startMs;
                    seqEnd   = ranges[idx].endMs;
                }
                if (seqStart == 0 && seqEnd == 0) {
                    seqEnd = 1 << 30;
                }
                Vector3f pos  = preset.position;
                Vector3f tgt  = preset.target;
                f32      roll = preset.staticRoll;

                const i32 sampleMs = focus ? focus->animation.TimeMs()
                                           : scene_->GetAnimationTime();
                preset.animator(pos, tgt, roll, sampleMs, seqStart, seqEnd);
                scene_->Camera().SetDirectPose(pos, tgt, roll);
            }
        }
    }

    auto* cmd = gfx_->GetImmediateContext();

    const bool useHdr = (renderMode_ == RenderMode::HD);
    const gfx::TextureHandle sceneTarget = useHdr ? target.hdrColor : target.color;

    auto srgbByteToLinear = [](u8 b) {
        const f32 f = b / 255.0f;
        return (f <= 0.04045f) ? (f / 12.92f)
                               : std::pow((f + 0.055f) / 1.055f, 2.4f);
    };

    auto acesInverse = [](f32 y) {
        if (y <= 0.0f) return 0.0f;
        if (y >= 1.0f) y = 0.9999f;
        const f32 A = 2.43f * y - 2.51f;
        const f32 B = 0.59f * y - 0.03f;
        const f32 C = 0.14f * y;
        if (std::abs(A) < 1e-6f) {
            return (std::abs(B) > 1e-6f) ? -C / B : 0.0f;
        }
        const f32 disc = B * B - 4.0f * A * C;
        if (disc < 0.0f) return 0.0f;
        const f32 r = std::sqrt(disc);
        const f32 x = (-B - r) / (2.0f * A);
        return x > 0.0f ? x : 0.0f;
    };
    const u32 bg = backgroundColor_.load();
    const u8  rB = static_cast<u8>(bg        & 0xFF);
    const u8  gB = static_cast<u8>((bg >> 8) & 0xFF);
    const u8  bB = static_cast<u8>((bg >> 16) & 0xFF);
    auto hdrClear = [&](u8 byte) {
        const f32 linTarget = srgbByteToLinear(byte);
        const f32 invExp    = (tonemapExposure_ > 1e-6f)
                                ? 1.0f / tonemapExposure_ : 1.0f;
        return acesInverse(linTarget) * invExp;
    };

    auto sdClear = [&](u8 byte) {
        return srgbByteToLinear(byte);
    };
    f32 clearColor[4] = {
        useHdr ? hdrClear(rB) : sdClear(rB),
        useHdr ? hdrClear(gB) : sdClear(gB),
        useHdr ? hdrClear(bB) : sdClear(bB),
        1.0f,
    };

    if (shadowService_ && shadowService_->IsEnabled()) {
        Matrix44f csmCamView, csmCamProj;
        {
            std::lock_guard<std::mutex> lock(dataMutex_);
            const f32 aspect= (target.height > 0)
                ? (f32)target.width / (f32)target.height : 1.0f;
            csmCamView = scene_->Camera().ViewLH();
            csmCamProj = scene_->Camera().ProjectionLH(aspect);
        }

        Vector3f lightDirWS = { -0.3f, 0.5f, -0.7f };
        if (auto* dnc = GetDncService(); dnc && dnc->HasAsset()) {
            const auto sample = dnc->SampleNow();
            if (sample.valid) lightDirWS = sample.worldDir;
        }

        Vector3f sceneCenter = { 0.0f, 0.0f, 0.0f };
        f32      sceneRadius = 150.0f;
        if (auto* hero = focusModel(); hero) {
            sceneCenter.x = hero->worldTransform.data[3][0];
            sceneCenter.y = hero->worldTransform.data[3][1];
            sceneCenter.z = hero->worldTransform.data[3][2];
        }
        if (scene_->Camera().GetMode() == Camera::Mode::Orbital) {
            sceneRadius = std::max(50.0f, scene_->Camera().GetDistance() * 0.6f);
        }
        shadowService_->Update(csmCamView, csmCamProj,
                               scene_->Camera().GetNearZ(),
                               scene_->Camera().GetFarZ(),
                               lightDirWS, sceneCenter, sceneRadius);
        shadow::ShadowPass(*this).Run(*shadowService_);
    }

    cmd->BeginRenderPass(sceneTarget, target.depth, clearColor, 1.0f, 0);
    cmd->SetViewport({0, 0, (f32)target.width, (f32)target.height, 0, 1});

    Matrix44f view, proj;
    {
        std::lock_guard<std::mutex> lock(dataMutex_);
        view = scene_->Camera().GetViewMatrix();
    }
    f32 aspect = (target.height > 0) ? (f32)target.width / (f32)target.height : 1.0f;
    proj = scene_->Camera().ProjectionRH(aspect);

    {
        render_detail::CbPerFrameDesc d;
        d.view         = view;
        d.projection   = proj;
        d.lightDir     = render_detail::NormalizedLightDir4(kDefaultLightDir);
        d.lightColor   = kGeosetLightColor;
        d.ambientColor = {kGeosetAmbientColor.x, kGeosetAmbientColor.y, kGeosetAmbientColor.z, 0.0f};
        render_detail::WriteCbPerFrame(gfx_.get(), cbPerFrame_, d);
    }

    if (showGrid_) debug_->RenderGrid();

    RenderGeosets(GeosetBucket::Opaque);
    if (showEvents_)    RenderSplatsBls();
    RenderGeosets(GeosetBucket::Transparent);
    if (showParticles_) RenderParticlesBls();
    if (showRibbons_)   RenderRibbons();
    if (showCollisions_) debug_->RenderCollisions();
    if (showLights_)     debug_->RenderLightMarkers();
    debug_->RenderViewCube();
    cmd->EndRenderPass();

    if (useHdr) RunTonemapPass(target);
}

void RenderService::RunTonemapPass(const RenderTarget& target) {
    if (tonemapPSO_ == gfx::PipelineHandle::Invalid) return;
    auto* cmd = gfx_->GetImmediateContext();

    const f32 clearLdr[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    cmd->BeginRenderPass(target.color, gfx::TextureHandle::Invalid,
                         clearLdr, 1.0f, 0);
    cmd->SetViewport({0, 0, (f32)target.width, (f32)target.height, 0, 1});

    if (tonemapPsCb_ != gfx::BufferHandle::Invalid) {
        if (void* mapped = gfx_->MapBuffer(tonemapPsCb_)) {
            f32 cb[4] = {tonemapExposure_, 0.0f, 0.0f, 0.0f};
            std::memcpy(mapped, cb, sizeof(cb));
            gfx_->UnmapBuffer(tonemapPsCb_);
        }
    }

    cmd->BindPipeline(tonemapPSO_);
    cmd->BindVertexBuffer(0, tonemapVB_, sizeof(f32) * 5);
    cmd->BindShaderResource(gfx::ShaderStage::Pixel, 0, target.hdrColor);
    cmd->BindSampler       (gfx::ShaderStage::Pixel, 0, tonemapSampler_);
    cmd->BindConstantBuffer(gfx::ShaderStage::Pixel, 1, tonemapPsCb_);
    cmd->Draw(3, 0);
    cmd->EndRenderPass();
}

void RenderService::Present(RenderTargetId targetId) {
    auto it = targets_.find(targetId);
    if (it != targets_.end() && it->second.swap != gfx::SwapChainHandle::Invalid)
        gfx_->Present(it->second.swap);
}

class GeosetPassBls : public BlsGeosetPass<GeosetPassBls> {
public:
    using BlsGeosetPass::BlsGeosetPass;

    bool IsAvailable() const {
        return rs_.blsSdProgram_ && rs_.blsPsoBuilder_;
    }

    void ComputeViewProj(Matrix44f& view, Matrix44f& proj) const {
        std::lock_guard<std::mutex> lock(rs_.dataMutex_);
        view = rs_.scene_->Camera().GetViewMatrix();
        const f32 aspect= (rs_.height_ > 0)
            ? static_cast<f32>(rs_.width_) / static_cast<f32>(rs_.height_) : 1.0f;
        proj = rs_.scene_->Camera().ProjectionRH(aspect);
    }

    void BindPassResources(gfx::IGFXCommandList*, bls::FrameInputs&) const {

    }

    bls::BaselineLights Baseline(const Matrix44f& view) const {

        if (auto* dnc = rs_.GetDncService();
            dnc && dnc->HasAsset() &&
            rs_.GetLightingMode() == LightingMode::InGame) {
            const auto sample = dnc->SampleNow();
            if (sample.valid) {

                const Vector3f dirVS = whiteout::transform_normal(
                    Vector3f{ -sample.worldDir.x, -sample.worldDir.y, -sample.worldDir.z },
                    view);
                return { sample.ambient, sample.diffuse, dirVS };
            }
        }

        return {
                    { kGeosetAmbientColor.x, kGeosetAmbientColor.y, kGeosetAmbientColor.z },
                    { kGeosetLightColor.x,   kGeosetLightColor.y,   kGeosetLightColor.z },
              { 0.0f, 0.0f, 1.0f },
        };
    }

    void DrawGeoset(const render_detail::GeosetRef& ref,
                    bls::FrameInputs&               frame,
                    const Matrix44f&                 ,
                    gfx::IGFXCommandList*           cmd,
                    i32                             lightCountForGeoset) {
        const auto& view_ = *ref.view;
        const auto& geo   = (*view_.geosets)[ref.idx];

        const GPUMaterial* mat = nullptr;
        const i32 matId= geo.materialId;
        if (matId >= 0 && matId < (i32)view_.materials->size())
            mat = &(*view_.materials)[matId];

        const f32 geoAlpha= geo.geosetAlpha * view_.parentVisibility;

        if (geoAlpha <= 0.0f) return;

        i32 numLayers = mat ? (i32)mat->cpu.layers.size() : 0;
        if (numLayers <= 0) numLayers = 1;

        const bool hasBones = render_detail::BindSdMeshGeometry(cmd, geo);

        const auto layout = hasBones
            ? bls::VertexLayoutKind::ParticleSDSkinned
            : bls::VertexLayoutKind::ParticleSD;

        struct LayerJob {
            render_detail::UnpackedLayer layer;
            bls::MatParams               mp;
            i32                          activeN = 0;
            bool                         unlit   = false;
            bool                         isOpaqueFading = false;
            bool                         valid   = false;
        };
        std::vector<LayerJob> jobs(numLayers);

        for (i32 li= 0; li < numLayers; ++li) {
            jobs[li].layer = render_detail::UnpackLayer(mat, li);
            const auto& layer = jobs[li].layer;
            const f32 combinedAlpha = geoAlpha * layer.alpha;
            if (combinedAlpha < 0.004f) continue;

            const bool isOpaqueFading =
                combinedAlpha < 0.99f && layer.filterMode <= FILTER_TRANSPARENT;
            i32 effectiveFilter = layer.filterMode;
            if (isOpaqueFading)
                effectiveFilter = FILTER_BLEND;

            bls::MatParams mp = bls::FromMdxLayer(effectiveFilter, layer.flags, bls::GxShaderID::SD);
            if (mp.alpha == bls::GxMatAlpha::Modulate) {
                mp.diffuseColor = {combinedAlpha, 1, 1, 1};
            } else {
                mp.diffuseColor = {geo.geosetColor.x, geo.geosetColor.y, geo.geosetColor.z, combinedAlpha};
            }
            const bool unlit = (mp.disables & bls::kDisableLighting) != 0;
            const i32 activeN = unlit ? 0 : lightCountForGeoset;

            jobs[li].mp = mp;
            jobs[li].activeN = activeN;
            jobs[li].unlit   = unlit;
            jobs[li].isOpaqueFading = isOpaqueFading;
            jobs[li].valid   = true;
        }

        auto issueDraw = [&](const LayerJob& job, const bls::MatParams& matParams) {
            frame.numLights = job.activeN;
            render_detail::ApplyTexAnimPaletteToFrame(frame, view_.texAnimPalette, job.layer.textureAnimationId);

            const auto rsLocal   = bls::MakeSdMeshRenderState(matParams, job.activeN, job.unlit, hasBones);
            const auto permLocal = bls::SelectPermutes(rsLocal);
            auto reqLocal        = bls::MakePsoRequest(rs_.blsSdProgram_,
                                                       layout,
                                                       matParams, permLocal);

            reqLocal.rtvFormat = rs_.SceneTargetFormat();
            auto pso = rs_.blsPsoBuilder_->GetOrBuild(reqLocal);
            if (pso == gfx::PipelineHandle::Invalid) return;
            cmd->BindPipeline(pso);

            cmd->BindVertexBuffer(0,
                render_detail::PickSlot0Vb(geo, job.layer.coordId),
                sizeof(Vertex));

            frame.world = view_.worldTransform;

            if (auto vs = bls::ScopedCb<bls::SdVsCbA>(rs_.gfx_.get(), rs_.blsSdVsCb_)) {
                bls::BuildSdVsCbA(*vs, frame, matParams);
            }
            if (auto ps = bls::ScopedCb<bls::SdPsCbA>(rs_.gfx_.get(), rs_.blsSdPsCb_)) {
                bls::BuildSdPsCbA(*ps, frame, matParams);
            }
            cmd->BindConstantBuffer(gfx::ShaderStage::Vertex, 0, rs_.blsSdVsCb_);
            cmd->BindConstantBuffer(gfx::ShaderStage::Pixel,  0, rs_.blsSdPsCb_);

            render_detail::BindLayerAlbedo(cmd, view_.textures, job.layer.textureId,
                                           rs_.textures_->GetDefaults().White,
                                           *rs_.samplers_);

            cmd->DrawIndexed(geo.indexCount);
        };

        for (i32 li= 0; li < numLayers; ++li) {
            if (!jobs[li].valid || !jobs[li].isOpaqueFading) continue;
            bls::MatParams prepass = jobs[li].mp;
            prepass.diffuseColor = {1.0f, 1.0f, 1.0f, 1.0f};
            prepass.disables &= ~bls::kDisableDepthWrite;
            prepass.disables |=  bls::kDisableBit8;
            issueDraw(jobs[li], prepass);
        }

        for (i32 li= 0; li < numLayers; ++li) {
            if (!jobs[li].valid) continue;
            issueDraw(jobs[li], jobs[li].mp);
        }
    }
};

bool RenderService::RenderGeosetsBls(GeosetBucket bucket) {
    return GeosetPassBls{*this, bucket}.Run();
}

class GeosetPassHd : public BlsGeosetPass<GeosetPassHd> {
public:
    using BlsGeosetPass::BlsGeosetPass;

    bool IsAvailable() const {
        return rs_.blsHdProgram_ && rs_.blsPsoBuilder_;
    }

    void ComputeViewProj(Matrix44f& view, Matrix44f& proj) const {

        const f32 aspect= (rs_.height_ > 0)
            ? static_cast<f32>(rs_.width_) / static_cast<f32>(rs_.height_) : 1.0f;
        std::lock_guard<std::mutex> lock(rs_.dataMutex_);
        view = rs_.scene_->Camera().ViewLH();
        proj = rs_.scene_->Camera().ProjectionLH(aspect);
    }

    void BindPassResources(gfx::IGFXCommandList* cmd, bls::FrameInputs& frame) {

        rs_.replaceables_->GetHdSwatchTexture();

        const bool useDayNight = rs_.iblDayNightLoaded_
                              && rs_.GetDncService() != nullptr
                              && rs_.GetLightingMode() == LightingMode::InGame;
        if (useDayNight) {
            const auto blend = rs_.GetDncService()->ComputeEnvMapBlend();

            const bool dayPrimary = blend.isDaytime;
            frame.envFromMipEnd  = dayPrimary ? rs_.iblDayMipEnd_   : rs_.iblNightMipEnd_;
            frame.envToMipEnd    = dayPrimary ? rs_.iblNightMipEnd_ : rs_.iblDayMipEnd_;
            frame.envTransitionT = blend.transitionT;
        } else {
            frame.envFromMipEnd  = rs_.iblProbeMipEnd_;
            frame.envToMipEnd    = rs_.iblProbeMipEnd_;
            frame.envTransitionT = 0.75f;
        }

        const gfx::SamplerHandle linWrap = rs_.samplers_->LinearWrap();
        cmd->BindSampler(gfx::ShaderStage::Pixel, 1, linWrap);
        cmd->BindSampler(gfx::ShaderStage::Pixel, 2, linWrap);
        cmd->BindSampler(gfx::ShaderStage::Pixel, 3, linWrap);

        gfx::TextureHandle from = gfx::TextureHandle::Invalid;
        gfx::TextureHandle to   = gfx::TextureHandle::Invalid;
        if (useDayNight) {
            const auto day   = rs_.textures_->GetOwned(RenderService::kIblDayProbeName);
            const auto night = rs_.textures_->GetOwned(RenderService::kIblNightProbeName);
            const auto blend = rs_.GetDncService()->ComputeEnvMapBlend();
            from = blend.isDaytime ? day   : night;
            to   = blend.isDaytime ? night : day;
        } else {
            from = rs_.textures_->GetOwned(RenderService::kIblFromProbeName);
            to   = rs_.textures_->GetOwned(RenderService::kIblToProbeName);
            if (to == gfx::TextureHandle::Invalid) to = from;
        }
        if (from != gfx::TextureHandle::Invalid)
            cmd->BindShaderResource(gfx::ShaderStage::Pixel, 13, from);
        if (to   != gfx::TextureHandle::Invalid)
            cmd->BindShaderResource(gfx::ShaderStage::Pixel, 14, to);
        const gfx::TextureHandle lut = rs_.textures_->GetOwned(
            RenderService::kIblSplitSumLutName);
        if (lut != gfx::TextureHandle::Invalid)
            cmd->BindShaderResource(gfx::ShaderStage::Pixel, 15, lut);

        if (rs_.shadowService_) {
            for (i32 c= 0; c < 3; ++c) {
                const gfx::TextureHandle sh = rs_.shadowService_->depthTarget(c);
                if (sh != gfx::TextureHandle::Invalid) {
                    cmd->BindShaderResource(gfx::ShaderStage::Pixel,
                                            10 + static_cast<u32>(c), sh);
                }
            }
        }
    }

    bls::BaselineLights Baseline(const Matrix44f& view) const {

        if (auto* dnc = rs_.GetDncService();
            dnc && dnc->HasAsset() &&
            rs_.GetLightingMode() == LightingMode::InGame) {
            const auto sample = dnc->SampleNow();
            if (sample.valid) {
                const Vector3f dirVS = whiteout::transform_normal(
                    Vector3f{ -sample.worldDir.x, -sample.worldDir.y, -sample.worldDir.z },
                    view);
                return { sample.ambient, sample.diffuse, dirVS };
            }
        }

        return {
                    { kHdBaselineAmbientColor.x, kHdBaselineAmbientColor.y, kHdBaselineAmbientColor.z },
                    { kHdBaselineLightColor.x,   kHdBaselineLightColor.y,   kHdBaselineLightColor.z },
              { 0.0f, 0.0f, -1.0f },
        };
    }

    void DrawGeoset(const render_detail::GeosetRef& ref,
                    bls::FrameInputs&               frame,
                    const Matrix44f&                 ,
                    gfx::IGFXCommandList*           cmd,
                    i32                             lightCountForGeoset) {
        const auto& view_ = *ref.view;
        const auto& geo   = (*view_.geosets)[ref.idx];

        const GPUMaterial* mat = nullptr;
        const i32 matId= geo.materialId;
        if (matId >= 0 && matId < (i32)view_.materials->size())
            mat = &(*view_.materials)[matId];

        const f32 geoAlpha= geo.geosetAlpha * view_.parentVisibility;

        if (geoAlpha <= 0.0f) return;

        i32 numLayers = mat ? (i32)mat->cpu.layers.size() : 0;
        if (numLayers <= 0) numLayers = 1;

        cmd->BindIndexBuffer(geo.ib, gfx::Format::R32_UINT);

        const bool hasTangents = (geo.tangentVb != gfx::BufferHandle::Invalid);
        if (hasTangents)
            cmd->BindVertexBuffer(1, geo.tangentVb, sizeof(Vector4f));

        const bool hasBones =
            (geo.boneVb != gfx::BufferHandle::Invalid) &&
            (geo.bonePaletteCb != gfx::BufferHandle::Invalid);
        if (hasBones) {
            const u32 boneSlot = hasTangents ? 2u : 1u;
            cmd->BindVertexBuffer(boneSlot, geo.boneVb, sizeof(BoneVertex));
            cmd->BindConstantBuffer(gfx::ShaderStage::Vertex, 3, geo.bonePaletteCb);
        }

        struct LayerJob {
            render_detail::UnpackedLayer layer;
            bls::MatParams               mp;
            const bls::BlsProgram*       program = nullptr;
            bls::GxShaderID              programShaderId = bls::GxShaderID::SD_on_HD;
            i32                          activeN = 0;
            bool                         unlit   = false;
            bool                         isOpaqueFading = false;
            bool                         valid   = false;
        };
        std::vector<LayerJob> jobs(numLayers);

        for (i32 li= 0; li < numLayers; ++li) {
            jobs[li].layer = render_detail::UnpackLayer(mat, li);
            const auto& layer = jobs[li].layer;
            f32 combinedAlpha = geoAlpha * layer.alpha;
            if (combinedAlpha < 0.004f) continue;
            const bool isOpaqueFading =
                combinedAlpha < 0.99f && layer.filterMode <= FILTER_TRANSPARENT;
            i32 effectiveFilter = layer.filterMode;
            if (isOpaqueFading)
                effectiveFilter = FILTER_BLEND;

            const bool isCrystalMaterial = (layer.shaderId == 24)
                                        && rs_.blsCrystalProgram_ != nullptr;
            const bool isHdMaterial =
                isCrystalMaterial
                || layer.shaderId == 1
                || layer.shaderId == 24;
            const bls::GxShaderID programShaderId =
                isCrystalMaterial ? bls::GxShaderID::Crystal
                : isHdMaterial    ? bls::GxShaderID::HD
                                  : bls::GxShaderID::SD_on_HD;
            const bls::BlsProgram* program =
                isCrystalMaterial ? rs_.blsCrystalProgram_
                : isHdMaterial    ? rs_.blsHdProgram_
                                  : rs_.blsSdOnHdProgram_;

            bls::MatParams mp = bls::FromMdxLayer(effectiveFilter, layer.flags, programShaderId);
            if (mp.alpha == bls::GxMatAlpha::Modulate) {
                mp.diffuseColor = {combinedAlpha, 1, 1, 1};
            } else {
                mp.diffuseColor = {geo.geosetColor.x, geo.geosetColor.y, geo.geosetColor.z, combinedAlpha};
            }
            mp.emissiveGain     = layer.emissiveGain;
            mp.fresnelTeamColor = layer.fresnelTeamColor;
            mp.fresnelOpacity   = layer.fresnelOpacity;
            mp.fresnelColor     = layer.fresnelColor;

            const bool unlit   = (mp.disables & bls::kDisableLighting) != 0;
            const i32 activeN = unlit ? 0 : lightCountForGeoset;

            jobs[li].mp = mp;
            jobs[li].program = program;
            jobs[li].programShaderId = programShaderId;
            jobs[li].activeN = activeN;
            jobs[li].unlit   = unlit;
            jobs[li].isOpaqueFading = isOpaqueFading;
            jobs[li].valid   = true;
        }

        auto issueHdDraw = [&](const LayerJob& job, const bls::MatParams& matParams) {
            const auto& layer = job.layer;
            const bool unlit  = job.unlit;
            const i32 activeN = job.activeN;
            const bls::GxShaderID programShaderId = job.programShaderId;
            const bls::BlsProgram* program = job.program;
            frame.numLights = activeN;
            render_detail::ApplyTexAnimPaletteToFrame(frame, view_.texAnimPalette, layer.textureAnimationId);
            {
            bls::RenderState rs;
            rs.shaderId       = programShaderId;
            rs.alphaMode      = static_cast<u8>(matParams.alpha);
            rs.numColors      = 0;
            rs.numTexCoords   = 1;

            rs.numTangents    = hasTangents ? 1 : 0;

            rs.numWeights     = hasBones ? 4 : 0;
            rs.numLights      = static_cast<u8>(activeN);
            rs.fogEnabled     = false;
            rs.depthWrite     = matParams.DepthWriteEnabled();
            rs.lightingEnabled= !unlit && activeN > 0;
            rs.prepass        = false;
            rs.shadows        = rs_.shadowService_ && rs_.shadowService_->IsEnabled();

            rs.teamColor      = (layer.teamColorMapId == kHdTeamColorActive)
                             || (layer.teamColorMapId >= 0);
            const i32 dbgMode     = rs_.hdDebugMode_.load();
            const bool debugActive = (dbgMode > 0);
            rs.debugShader = debugActive;
            auto perm = bls::SelectPermutes(rs);

            bls::PsoRequest req{};
            req.program   = program;
            req.vsIndex   = perm.vs;
            req.psIndex   = perm.ps;
            req.material  = matParams;

            if (hasBones) {
                req.layout = hasTangents
                    ? bls::VertexLayoutKind::MeshHDSkinned
                    : bls::VertexLayoutKind::MeshHDSkinnedNoTangent;
            } else {
                req.layout = hasTangents
                    ? bls::VertexLayoutKind::MeshHDTangent
                    : bls::VertexLayoutKind::ParticleSD;
            }
            req.topology  = gfx::PrimitiveTopology::TriangleList;
            req.rtvFormat = RenderService::kHdrSceneFormat;
            req.dsvFormat = gfx::Format::D24_UNORM_S8_UINT;
            req.lhClipSpace = true;
            auto pso = rs_.blsPsoBuilder_->GetOrBuild(req);
            if (pso == gfx::PipelineHandle::Invalid) return;
            cmd->BindPipeline(pso);

            cmd->BindVertexBuffer(0,
                render_detail::PickSlot0Vb(geo, layer.coordId),
                sizeof(Vertex));

            frame.world = view_.worldTransform;

            if (auto vs = bls::ScopedCb<bls::HdVsCb>(rs_.gfx_.get(), rs_.blsHdVsCb_)) {
                bls::BuildHdVsCb(*vs, frame, matParams);
            }
            cmd->BindConstantBuffer(gfx::ShaderStage::Vertex, 2, rs_.blsHdVsCb_);

            if (rs_.shadowService_ && rs_.blsHdShadowCb_ != gfx::BufferHandle::Invalid) {
                if (auto sc = bls::ScopedCb<bls::HdShadowCascadesCb>(rs_.gfx_.get(),
                                                                      rs_.blsHdShadowCb_)) {
                    rs_.shadowService_->FillVsCb(*sc);
                }
                cmd->BindConstantBuffer(gfx::ShaderStage::Vertex, 1, rs_.blsHdShadowCb_);
            }

            if (program == rs_.blsHdProgram_ ||
                program == rs_.blsCrystalProgram_) {
                if (auto ps = bls::ScopedCb<bls::HdPsCb>(rs_.gfx_.get(), rs_.blsHdPsCb_)) {
                    bls::BuildHdPsCb(*ps, frame, matParams);
                }
                cmd->BindConstantBuffer(gfx::ShaderStage::Pixel, 2, rs_.blsHdPsCb_);

                if (auto dbg = bls::ScopedCb<bls::DebugVisCb>(rs_.gfx_.get(), rs_.blsHdDebugVisCb_)) {

                    u32 enabled   = 0;
                    i32      psMode    = dbgMode;
                    Vector3f overrideA = {0, 0, 0};
                    if (dbgMode >= 5 && dbgMode <= 7) {
                        enabled = 1;
                        psMode  = 0;
                        overrideA = (dbgMode == 5) ? Vector3f{1, 1, 1}
                                   : (dbgMode == 6) ? Vector3f{0.5f, 0.5f, 0.5f}
                                                    : Vector3f{0, 0, 0};
                    }
                    dbg->enabledShaders = enabled;

                    const u32 modeBits = static_cast<u32>(psMode);
                    std::memcpy(&dbg->debugMode, &modeBits, sizeof(f32));
                    dbg->_p0[0] = dbg->_p0[1] = 0.0f;
                    dbg->overrideAlbedo = overrideA; dbg->_p1 = 0.0f;
                    dbg->overrideOrm    = {0, 0, 0}; dbg->_p2 = 0.0f;
                }
                cmd->BindConstantBuffer(gfx::ShaderStage::Pixel, 3, rs_.blsHdDebugVisCb_);
            } else {
                if (auto ps = bls::ScopedCb<bls::SdOnHdPsCb>(rs_.gfx_.get(), rs_.blsSdOnHdPsCb_)) {
                    bls::BuildSdOnHdPsCb(*ps, frame, matParams);
                }
                cmd->BindConstantBuffer(gfx::ShaderStage::Pixel, 2, rs_.blsSdOnHdPsCb_);
            }

            if (rs_.blsHdShadowCountCb_ != gfx::BufferHandle::Invalid) {
                if (auto cnt = bls::ScopedCb<bls::SdOnHdShadowCascadeCountCb>(
                        rs_.gfx_.get(), rs_.blsHdShadowCountCb_)) {
                    const i32 n = (rs_.shadowService_ && rs_.shadowService_->IsEnabled())
                                      ? rs_.shadowService_->cascadeCount()
                                      : 0;
                    const u32 bits = static_cast<u32>(n);
                    std::memcpy(&cnt->numCascades, &bits, sizeof(f32));
                    cnt->_pad[0] = cnt->_pad[1] = cnt->_pad[2] = 0.0f;
                }
                cmd->BindConstantBuffer(gfx::ShaderStage::Pixel, 1, rs_.blsHdShadowCountCb_);
            }

            auto bindMaterialTex = [&](u32 slot, i32 texId,
                                        gfx::TextureHandle fallback,
                                        u32* outWrap) {
                if (texId >= 0 && view_.textures) {
                    const gfx::TextureHandle h = view_.textures->Get(texId);
                    if (h != gfx::TextureHandle::Invalid) {
                        cmd->BindShaderResource(gfx::ShaderStage::Pixel, slot, h);
                        if (outWrap) *outWrap = view_.textures->WrapFlags(texId) & kSamplerWrapBitsMask;
                        return true;
                    }
                }
                cmd->BindShaderResource(gfx::ShaderStage::Pixel, slot, fallback);
                return false;
            };

            const auto& defs = rs_.textures_->GetDefaults();
            u32 wrapFlags = kSamplerWrapBitsMask;
            bindMaterialTex(0, layer.textureId,      defs.White,      &wrapFlags);
            bindMaterialTex(1, layer.normalMapId,    defs.FlatNormal, nullptr);
            bindMaterialTex(2, layer.ormMapId,       defs.NeutralOrm, nullptr);
            bindMaterialTex(3, layer.emissiveMapId,  defs.Black,      nullptr);

            if (layer.teamColorMapId == kHdTeamColorActive) {
                cmd->BindShaderResource(gfx::ShaderStage::Pixel, 4,
                                        rs_.replaceables_->GetHdSwatchTexture());
            } else if (layer.teamColorMapId >= 0) {
                bindMaterialTex(4, layer.teamColorMapId, defs.Black, nullptr);
            } else {
                cmd->BindShaderResource(gfx::ShaderStage::Pixel, 4, defs.Black);
            }
            cmd->BindSampler(gfx::ShaderStage::Pixel, 0, rs_.samplers_->WrapVariant(wrapFlags));

            cmd->DrawIndexed(geo.indexCount);
            }
        };

        for (i32 li= 0; li < numLayers; ++li) {
            if (!jobs[li].valid || !jobs[li].isOpaqueFading) continue;
            bls::MatParams prepass = jobs[li].mp;
            prepass.diffuseColor = {1.0f, 1.0f, 1.0f, 1.0f};
            prepass.disables &= ~bls::kDisableDepthWrite;
            prepass.disables |=  bls::kDisableBit8;
            issueHdDraw(jobs[li], prepass);
        }

        for (i32 li= 0; li < numLayers; ++li) {
            if (!jobs[li].valid) continue;
            issueHdDraw(jobs[li], jobs[li].mp);
        }
    }
};

bool RenderService::RenderGeosetsHd(GeosetBucket bucket) {
    return GeosetPassHd{*this, bucket}.Run();
}

void RenderService::RenderGeosets(GeosetBucket bucket) {

    if (renderMode_ == RenderMode::HD) {
        RenderGeosetsHd(bucket);
    } else {
        RenderGeosetsBls(bucket);
    }
}

void RenderService::SnapCameraToFace(i32 faceIndex) {
    if (faceIndex < 0 || faceIndex > 5) return;

    static constexpr Vector3f kFaceNormalsMax[6] = {
        { 0, 1, 0},
        { 0,-1, 0},
        {-1, 0, 0},
        { 1, 0, 0},
        { 0, 0, 1},
        { 0, 0,-1},
    };
    Vector3f n = CoordinateSystem::ConvertDirection(
        CoordSpace::Max, CoordinateSystem::Default(), kFaceNormalsMax[faceIndex]);

    constexpr f32 kTopBottomPitch = 1.55f;
    if (std::abs(n.z) > 0.99f) {

        scene_->Camera().SetYaw(Camera::kDefaultYaw);
        scene_->Camera().SetPitch(n.z > 0 ? kTopBottomPitch : -kTopBottomPitch);
    } else {
        scene_->Camera().SetYaw(std::atan2(n.y, n.x));
        scene_->Camera().SetPitch(0.0f);
    }
}

}
