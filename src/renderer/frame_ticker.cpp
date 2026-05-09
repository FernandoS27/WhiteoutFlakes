// ============================================================================
// FrameTicker — extracted from RenderService.
//
// Drives one frame's worth of scene updates: attachment loading, animation
// evaluation, particle / PE1 / ribbon simulation, bone-palette CB writes.
// Reaches into RenderService::Impl directly via friend access for the same
// reasons the other in-tree subsystems do (DebugRenderer, SpnSpawner,
// shadow::ShadowPass) — see render_service_impl.h.
// ============================================================================

#include "frame_ticker.h"

#include "animation/actor_eval_context.h"
#include "bls/scoped_cb.h"
#include "bls/bls_cb_layout.h"
#include "bls/bls_draw_helpers.h"
#include "model/model_instance.h"
#include "model/model_template.h"
#include "particle/plane_emitter.h"
#include "render_service.h"
#include "render_service_impl.h"
#include "assets/replaceable_texture_manager.h"
#include "../io/mdx_model_adapter.h"

#include <vector>

namespace whiteout::flakes::renderer {

using namespace ::whiteout::flakes::renderer::model;
using namespace ::whiteout::flakes::renderer::animation;
using namespace ::whiteout::flakes::renderer::effects;
using namespace ::whiteout::flakes::renderer::debug;
using namespace ::whiteout::flakes::renderer::bls;
using namespace ::whiteout::flakes::renderer::shadow;

void FrameTicker::Tick(f32 dt) {
    rs_.Scene().Templates().Tick();
    UpdateAttachments();
    EvaluateTopLevelActors();
    EvaluatePE1Children();
    UpdateAnimation();
    UpdateParticles(dt);
    UpdatePE1(dt);
    UpdateRibbons(dt);
}

void FrameTicker::UpdateAttachments() {
    std::vector<u32> handles;
    for (auto& [h, mi] : rs_.Scene().Actors().All()) handles.push_back(h);

    for (u32 h : handles) {
        auto* mi = rs_.Scene().Actors().Find(h);
        if (!mi) continue;

        for (auto& slot : mi->attachmentSlots) {
            if (slot.loaded || slot.config.modelPath.empty()) continue;

            auto tmpl = rs_.Scene().Templates().GetOrLoadAsync(slot.config.modelPath);
            if (!tmpl) continue;

            slot.loaded = true;

            u32 childH = rs_.Scene().NextActorIdRef()++;
            auto child = std::make_unique<Actor>();
            child->handle = childH;
            child->parent = mi->handle;
            child->isPE1Child = true;
            child->pe1Depth = mi->pe1Depth + 1;
            child->animation.Bind(tmpl->adapter);
            child->animation.SetBirthTimeMs(rs_.Scene().GetAnimationTime());

            auto seqs = tmpl->adapter->GetSequences();
            if (!seqs.empty())
                child->animation.SetActiveSequenceIndex(rand() % (i32)seqs.size());

            rs_.Loader().StageActor(child.get(), tmpl);
            rs_.Scene().Actors().All()[childH] = std::move(child);
            slot.childModelHandle = childH;
        }
    }
}

void FrameTicker::EvaluateTopLevelActors() {

    struct ActorEval {
        Actor* actor;
        std::shared_ptr<IAnimationSource> adapter;
        Matrix44f worldTransform;
        i32 seqIdx;
        i32 localTimeMs;
        i32 globalTimeMs;
    };
    std::vector<ActorEval> toEval;
    const ActorEvalContext ctx = rs_.MakeActorEvalContext();

    {
        const i32 now = ctx.sceneAnimationTimeMs;
        for (auto& [h, mi] : rs_.Scene().Actors().All()) {
            if (mi->isPE1Child) continue;
            if (mi->externallyDriven) continue;
            if (!mi->animation.HasSource()) continue;
            const i32 localTime  = mi->animation.TimeMs();
            const i32 globalTime = now - mi->animation.BirthTimeMs();
            toEval.push_back({mi.get(), mi->animation.Source(), mi->worldTransform,
                              mi->animation.ActiveSequenceIndex(),
                              localTime, globalTime});
        }
    }

    for (auto& ae : toEval) {
        FrameState fs = ae.adapter->Evaluate(ae.seqIdx, ae.localTimeMs, ae.globalTimeMs,
                                             ae.worldTransform, ctx.camPos);
        ae.actor->ApplyFrameState(fs, ae.localTimeMs, ctx);
    }
}

void FrameTicker::EvaluatePE1Children() {
    struct ChildEval {
        Actor* actor;
        std::shared_ptr<IAnimationSource> adapter;
        i32 localTimeMs;
        i32 seqIdx;
        i32 globalTimeMs;
        Matrix44f worldTransform;
    };
    std::vector<ChildEval> toEval;
    const ActorEvalContext ctx = rs_.MakeActorEvalContext();

    {
        i32 timeMs = ctx.sceneAnimationTimeMs;
        for (auto& [h, mi] : rs_.Scene().Actors().All()) {
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
            toEval.push_back({mi.get(), mi->animation.Source(), localTime, seqIdx,
                              globalTime, mi->worldTransform});
        }
    }

    for (auto& ce : toEval) {
        FrameState fs = ce.adapter->Evaluate(ce.seqIdx, ce.localTimeMs, ce.globalTimeMs,
                                             ce.worldTransform, ctx.camPos);
        ce.actor->ApplyFrameState(fs, ce.localTimeMs, ctx);
    }
}

void FrameTicker::UpdateAnimation() {

    for (auto& [h, miPtr] : rs_.Scene().Actors().All()) {
        auto* mi = miPtr.get();
        if (!mi->render.skinning.HasSkeleton() || !mi->render.skinning.IsReady()) continue;
        if (mi->parentVisibility <= 0.02f) continue;

        mi->render.skinning.ComputeOffsetMatrices();

        for (auto& geo : mi->render.gpuGeosets) {
            if (geo.bonePaletteCb == gfx::BufferHandle::Invalid) continue;
            if (auto bp = bls::ScopedCb<bls::BonePaletteCb>(rs_.Pipeline().Gfx(), geo.bonePaletteCb)) {

                constexpr i32 kSlots = bls::kMaxBones;
                static thread_local Matrix44f staging[kSlots];
                mi->render.skinning.ComputeGeosetPalette(geo.geosetId, staging, kSlots);
                bls::BuildBonePalette(*bp, staging, kSlots);
            }
        }
    }
}

void FrameTicker::UpdateParticles(f32 dt) {
    rs_.Particles().Simulate(dt);
    rs_.Splats().Tick();
}

void FrameTicker::UpdatePE1(f32 dt) {
    std::vector<u32> toRemove;

    std::vector<u32> handles;
    for (auto& [h, mi] : rs_.Scene().Actors().All()) handles.push_back(h);

    for (u32 h : handles) {
        auto* mi = rs_.Scene().Actors().Find(h);
        if (!mi) continue;
        if (mi->pe1Depth >= SceneManager::kMaxPE1Depth) continue;
        if (!mi->render.pe1.HasEmitters()) continue;
        if (mi->parentVisibility <= 0.02f) continue;

        auto result = mi->render.pe1.Simulate(dt, rs_.Scene().NextActorIdRef());

        for (auto& birth : result.born) {
            if (rs_.Scene().PE1InstanceCountRef() >= SceneManager::kMaxPE1Instances) continue;
            auto* cfg = mi->render.pe1.GetConfig(birth.emitterId);
            if (!cfg) continue;
            auto tmpl = rs_.Scene().Templates().GetOrLoadAsync(cfg->modelPath);
            if (!tmpl) continue;

            auto child = std::make_unique<Actor>();
            child->handle = birth.handle;
            child->parent = mi->handle;
            child->worldTransform = birth.worldTransform;
            child->isPE1Child = true;
            child->pe1Depth = mi->pe1Depth + 1;
            child->animation.Bind(tmpl->adapter);
            child->animation.SetBirthTimeMs(rs_.Scene().GetAnimationTime());

            rs_.Loader().StageActor(child.get(), tmpl);
            rs_.Scene().Actors().All()[birth.handle] = std::move(child);
            rs_.Scene().PE1InstanceCountRef()++;
        }

        for (u32 childH : result.died) toRemove.push_back(childH);

        for (auto& [childH, tm] : result.transforms) {
            if (auto* c = rs_.Scene().Actors().Find(childH)) c->worldTransform = tm;
        }
    }

    for (u32 rh : toRemove) {
        auto it = rs_.Scene().Actors().All().find(rh);
        if (it != rs_.Scene().Actors().All().end()) {
            rs_.Replaceables().UnregisterModel(*it->second);
            it->second->ReleaseGPU(*rs_.Pipeline().Gfx());
            rs_.Scene().Actors().All().erase(it);
            rs_.Scene().PE1InstanceCountRef()--;
        }

        rs_.Particles().RemoveModel(rh);
    }

    rs_.Spn().Tick(rs_.Scene().GetAnimationTime());
}

void FrameTicker::UpdateRibbons(f32 dt) {
    for (auto& [h, mi] : rs_.Scene().Actors().All()) {
        if (mi->parentVisibility <= 0.02f) continue;
        mi->render.ribbons.Simulate(dt);
    }
}

}
