// ============================================================================
// FrameTicker — per-frame scene-update orchestration.
//
// Drives one frame's worth of scene updates: attachment loading, animation
// evaluation, particle / PE1 / ribbon simulation, bone-palette CB writes.
// Uses only the public RenderService accessors.
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
    rs_.Replaceables().RebakeDirtyActors();
    UpdateAttachments();
    EvaluateActorTree();
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

            auto* child = rs_.Loader().SpawnChild(*mi, ActorRole::Attachment, tmpl);
            if (!child) continue;
            auto seqs = tmpl->adapter->GetSequences();
            if (!seqs.empty())
                child->animation.SetActiveSequenceIndex(rand() % (i32)seqs.size());

            slot.loaded           = true;
            slot.childModelHandle = child->handle;
        }
    }
}

void FrameTicker::EvaluateActorTree() {
    const ActorEvalContext ctx = rs_.MakeActorEvalContext();

    // Walk every top-level actor (Unit + External). External actors are
    // evaluated by the host (Max plugin) directly, so we only RECURSE into
    // their children — we don't re-evaluate the External itself here. Unit
    // actors are evaluated normally. The top-level actor's actorTimeMs is
    // threaded down so Attachment/PE1 children pause when their ancestor
    // pauses (playbackSpeed=0).
    std::vector<u32> tops;
    tops.reserve(rs_.Scene().Actors().All().size());
    for (auto& [h, mi] : rs_.Scene().Actors().All()) {
        if (!mi->IsChild()) tops.push_back(h);
    }
    for (u32 h : tops) {
        if (auto* a = rs_.Scene().Actors().Find(h))
            EvaluateActorTreeRec(*a, ctx, a->cursor.actorTimeMs);
    }
}

void FrameTicker::SilenceCornEmittersRec(Actor& actor) {
    rs_.CornEffects().SetOwningAgentVisibilityForModel(actor.handle, false);
    for (u32 ch : actor.children) {
        if (auto* c = rs_.Scene().Actors().Find(ch))
            SilenceCornEmittersRec(*c);
    }
}

void FrameTicker::EvaluateActorTreeRec(Actor& actor, const ActorEvalContext& ctx,
                                       i32 ancestorClock) {
    if (actor.IsChild() && actor.parentVisibility <= 0.02f) {
        // Hidden subtree: still tell descendants' corn fx emitters they're
        // invisible so they don't keep playing on stale state. Other
        // per-frame work (animation eval, particle sim, ribbon sim) is
        // already gated elsewhere on parentVisibility.
        SilenceCornEmittersRec(actor);
        return;
    }

    if (actor.role != ActorRole::External && actor.animation.HasSource()) {
        i32 localTimeMs;
        i32 globalTimeMs;
        const i32 seqIdx = actor.animation.ActiveSequenceIndex();

        if (actor.role == ActorRole::Unit) {
            // Top-level: trust the actor's own clock (set by Actor::Advance).
            localTimeMs  = actor.animation.TimeMs();
            globalTimeMs = ctx.sceneAnimationTimeMs - actor.animation.BirthTimeMs();
        } else {
            // Child: derive local time from a clock chosen by role.
            //   - Attachment / PE1: use ancestor's actorTimeMs so paused
            //     parents pause their visual children's animation cursors.
            //   - SPN: keep using the wall clock — SPN children have a
            //     wall-clock expiry that the spawner manages, and decoupling
            //     the cursor from that expiry would let visuals run past the
            //     scheduled despawn time.
            const i32 clock = (actor.role == ActorRole::SPN)
                                  ? ctx.sceneAnimationTimeMs
                                  : ancestorClock;
            i32 localTime = clock - actor.animation.BirthTimeMs();
            if (localTime < 0) localTime = 0;
            const auto seqs = actor.animation.Sequences();
            if (!seqs.empty()) {
                const i32 boundedSeq = seqIdx % (i32)seqs.size();
                const auto& seq = seqs[boundedSeq];
                const i32 dur = seq.endMs - seq.startMs;
                if (dur > 0) {
                    if (seq.nonLooping && !actor.ignoreNonLooping)
                        localTime = seq.startMs + (std::min)(localTime, dur);
                    else
                        localTime = seq.startMs + (localTime % dur);
                }
            }
            localTimeMs  = localTime;
            globalTimeMs = localTime;
        }

        FrameState fs = actor.animation.Source()->Evaluate(
            seqIdx, localTimeMs, globalTimeMs, actor.worldTransform, ctx.camPos);
        actor.ApplyFrameState(fs, localTimeMs, ctx);
    }

    // Recurse — copy the list because ApplyFrameState may have mutated
    // children's state (worldTransform, parentVisibility) but cannot add or
    // remove children mid-walk in the current model. The copy guards against
    // any future change to that contract.
    auto childList = actor.children;
    for (u32 ch : childList) {
        if (auto* c = rs_.Scene().Actors().Find(ch))
            EvaluateActorTreeRec(*c, ctx, ancestorClock);
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
    // CornFx deliberately is NOT ticked here — its cornflakes runtime
    // emits GPU draws inline during runtime->tick() via the backend's
    // submit(), so the tick must run inside a render pass. RenderPipeline
    // calls CornEffects().Simulate(dt) from its corn fx pass; we just stash
    // dt for the pipeline to pick up.
    rs_.CornEffects().SetPendingDt(dt);
}

void FrameTicker::UpdatePE1(f32 dt) {
    std::vector<u32> toRemove;

    std::vector<u32> handles;
    for (auto& [h, mi] : rs_.Scene().Actors().All()) handles.push_back(h);

    for (u32 h : handles) {
        auto* mi = rs_.Scene().Actors().Find(h);
        if (!mi) continue;
        if (mi->treeDepth >= effects::kMaxPE1Depth) continue;
        if (!mi->render.pe1.HasEmitters()) continue;
        if (mi->parentVisibility <= 0.02f) continue;

        auto result = mi->render.pe1.Simulate(dt,
            [&] { return rs_.Scene().AllocActorId(); });

        for (auto& birth : result.born) {
            if (rs_.Scene().PE1InstanceCount() >= effects::kMaxPE1Instances) continue;
            auto* cfg = mi->render.pe1.GetConfig(birth.emitterId);
            if (!cfg) continue;
            auto tmpl = rs_.Scene().Templates().GetOrLoadAsync(cfg->modelPath);
            if (!tmpl) continue;

            auto* child = rs_.Loader().SpawnChild(*mi, ActorRole::PE1, tmpl,
                                                  birth.worldTransform, birth.handle);
            if (!child) continue;
        }

        for (u32 childH : result.died) toRemove.push_back(childH);

        for (auto& [childH, tm] : result.transforms) {
            if (auto* c = rs_.Scene().Actors().Find(childH)) c->worldTransform = tm;
        }
    }

    for (u32 rh : toRemove) rs_.Loader().DestroyActor(rh);

    rs_.Spn().Tick(rs_.Scene().GetAnimationTime());
}

void FrameTicker::UpdateRibbons(f32 dt) {
    for (auto& [h, mi] : rs_.Scene().Actors().All()) {
        if (mi->parentVisibility <= 0.02f) continue;
        mi->render.ribbons.Simulate(dt);
    }
}

}
