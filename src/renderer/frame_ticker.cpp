// ============================================================================
// FrameTicker — per-frame scene-update orchestration.
//
// Drives one frame's worth of scene updates: attachment loading, animation
// evaluation, particle / ribbon simulation, bone-palette CB writes.
// Uses only the public RenderService accessors.
// ============================================================================

#include "frame_ticker.h"
#include "perf_zone.h"

#include "../io/mdx_model_adapter.h"
#include "animation/actor_eval_context.h"
#include "assets/asset_manager.h"
#include "assets/replaceable_texture_manager.h"
#include "bls/bls_cb_layout.h"
#include "bls/bls_draw_helpers.h"
#include "bls/scoped_cb.h"
#include "model/model_instance.h"
#include "model/model_template.h"
#include "particle/child_model_emitter.h"
#include "particle/d3_emitter.h"
#include "particle/model_particle_emitter.h"
#include "particle/particle2_emitter.h"
#include "particle/rnd_seed.h"
#include "render_service.h"
#include "render_service_impl.h"

#include <algorithm>
#include <vector>

namespace whiteout::flakes::renderer {

using namespace ::whiteout::flakes::renderer::model;
using namespace ::whiteout::flakes::renderer::animation;
using namespace ::whiteout::flakes::renderer::effects;
using namespace ::whiteout::flakes::renderer::debug;
using namespace ::whiteout::flakes::renderer::bls;
using namespace ::whiteout::flakes::renderer::shadow;

void FrameTicker::Tick(f32 dt) {
    Tick(rs_.DefaultScene(), dt);
}

void FrameTicker::Tick(SceneManager& scene, f32 dt) {
    WDX_CPU_ZONE("FrameTicker::Tick");
    // Publish the scene being ticked so Scene()/Particles()/CornEffects()/Spn()
    // and MakeActorEvalContext() resolve to it, then restore the default scene.
    rs_.SetActiveScene(scene);
    struct Restore {
        RenderService* rs;
        ~Restore() { rs->SetActiveScene(rs->DefaultSceneId()); }
    } restore{&rs_};

    // The scene owns the transport, and this is the half of the frame that
    // drives the effect simulations. Scaling dt here is what makes a pause
    // reach particles, PE1, ribbons and corn-fx — SceneManager::Update only
    // covers the animation clocks. Reading it from the scene rather than
    // taking a parameter keeps the two halves from disagreeing when a host
    // drives them separately, which is the normal arrangement.
    const f32 sdt = scene.EffectiveDt(dt);
    {
        // Pump the AssetManager's render-thread half: drains the
        // prepared queue (CPU-decoded texture / particle / child-MDX
        // bytes), creates GPU textures, and swaps slot payloads. Runs
        // at the top of Tick so freshly-applied assets land before the
        // per-frame replaceables / attachment / PE1 work reads them.
        WDX_CPU_ZONE("Assets.Commit");
        rs_.Assets().CommitPrepared();
    }
    {
        WDX_CPU_ZONE("RebakeDirtyActors");
        rs_.Replaceables().RebakeDirtyActors();
    }
    {
        WDX_CPU_ZONE("UpdateAttachments");
        UpdateAttachments();
    }
    {
        WDX_CPU_ZONE("EvaluateActorTree");
        EvaluateActorTree(dt);
    }
#if WDX_ENABLE_D3
    {
        WDX_CPU_ZONE("DriveD3Attachments");
        DriveD3Attachments();
    }
#endif
    {
        WDX_CPU_ZONE("UpdateAnimation");
        UpdateAnimation();
    }
    {
        WDX_CPU_ZONE("UpdateParticles");
        UpdateParticles(sdt);
    }
    {
        // Billboards and child models are simulated together in
        // UpdateParticles; this turns the child-model half's output into
        // actors. Separate only because actor lifetime is not the particle
        // service's business.
        WDX_CPU_ZONE("DriveChildModels");
        DriveChildModels();
    }
    {
        WDX_CPU_ZONE("UpdateRibbons");
        UpdateRibbons(sdt);
    }
    // Deliberately NOT SetPaused(): the emitter's paused flag feeds
    // ShouldBeSpawning, and an effect going inactive triggers cornflakes'
    // own reset-and-reinitialise, which respawns the effect from frame
    // zero — the opposite of holding it still. A zero dt freezes the sim
    // while leaving the emitter active, so the particles stay exactly
    // where they were and keep drawing.
}

void FrameTicker::UpdateAttachments() {
    // Sorted, not just snapshotted. SpawnChild takes its handle from
    // AllocActorId(), so walking in map order made *which child gets which
    // handle* a function of container iteration order. Sorting inside
    // BuildDrawLists cannot repair that — the sort key would itself be
    // unstable. With this, a child's handle is a function of the parent handle
    // and the slot index.
    std::vector<u32> handles;
    handles.reserve(rs_.Scene().Actors().All().size());
    for (auto& [h, mi] : rs_.Scene().Actors().All())
        handles.push_back(h);
    std::sort(handles.begin(), handles.end());

    for (u32 h : handles) {
        auto* mi = rs_.Scene().Actors().Find(h);
        if (!mi)
            continue;

        for (i32 si = 0; si < (i32)mi->attachmentSlots.size(); ++si) {
            auto& slot = mi->attachmentSlots[si];
            if (slot.loaded || slot.config.modelPath.empty())
                continue;

            // Acquire the AssetManager slot on first visit (later
            // frames reuse the cached SlotId). The slot's payload is
            // null until the host pump finishes the fetch + parse;
            // skip this frame in that case.
            if (slot.assetSlot == 0) {
                slot.assetSlot = rs_.Assets().Acquire(
                    assets::AssetKind::Model, assets::kSoleSubKind, slot.config.modelPath);
            }
            auto tmpl = rs_.Assets().ChildModelOf(slot.assetSlot);
            if (!tmpl)
                continue;

            auto* child = rs_.Loader().SpawnChild(*mi, ActorRole::Attachment, tmpl);
            if (!child)
                continue;
            child->spawnSlotIndex = si;
            auto seqs = tmpl->adapter->GetSequences();
            if (!seqs.empty()) {
                // See ApplyAttachmentStates: same unseeded-rand() bug, and this
                // one fires for *any* model with attachments, on the very first
                // frame. Keyed on (parent, slot) so it is a function of the
                // scene rather than of global RNG state.
                const u32 pick = particle::MixSeed(mi->handle, (u32)si);
                child->animation.SetActiveSequenceIndex((i32)(pick % (u32)seqs.size()));
            }

            slot.loaded = true;
            slot.childModelHandle = child->handle;
        }
    }
}

#if WDX_ENABLE_D3
void FrameTicker::DriveD3Attachments() {
    // Sorted for the reason UpdateAttachments is: SpawnChildFromSource takes
    // its handle from AllocActorId, so walking in map order would make which
    // child gets which handle a function of container iteration.
    std::vector<u32> handles;
    handles.reserve(rs_.Scene().Actors().All().size());
    for (auto& [h, mi] : rs_.Scene().Actors().All())
        if (!mi->d3Attachments.Empty())
            handles.push_back(h);
    if (handles.empty())
        return;
    std::sort(handles.begin(), handles.end());

    const i32 clock = rs_.Scene().GetAnimationTime();
    for (u32 h : handles) {
        auto* mi = rs_.Scene().Actors().Find(h);
        if (!mi)
            continue;
        // The sequence the actor just left takes its effects with it. Drained
        // ahead of the new sequence's spawns so a clip that re-enters and
        // re-fires the same attachment gets a fresh actor rather than racing
        // its own corpse.
        for (u32 dead : mi->d3Attachments.TakeExpired())
            rs_.Loader().DestroyActor(dead);

        for (const auto& p : mi->d3Attachments.TakePending()) {
            if (p.existing != 0) {
                // Already spawned once by this attachment. The engine would
                // make a second ACD; a looping viewer would then grow one per
                // lap, so the standing child is replayed from its own frame
                // zero instead — ApplyAttachmentStates' rule, for the reason
                // it gives.
                if (auto* child = rs_.Scene().Actors().Find(p.existing))
                    child->animation.SetBirthTimeMs(clock);
                continue;
            }
            Actor* child = rs_.Loader().SpawnD3ChildActor(*mi, p.snoActor, p.bone, p.offset);
            mi->d3Attachments.NoteChildSpawned(p.sequence, p.entry, child ? child->handle : 0);
        }
    }
}
#endif

void FrameTicker::EvaluateActorTree(f32 dt) {
    ActorEvalContext ctx = rs_.MakeActorEvalContext();
    // The *unscaled* delta on purpose: a rate-limited IK goal and a turret's
    // slew are real-world motion, so a paused actor's feet still settle onto
    // the ground instead of freezing mid-step.
    ctx.frameDtMs = (dt > 0.0f) ? static_cast<i32>(dt * 1000.0f + 0.5f) : 0;

    // Walk every top-level actor (Unit + External). External actors are
    // evaluated by the host (Max plugin) directly, so we only RECURSE into
    // their children — we don't re-evaluate the External itself here. Unit
    // actors are evaluated normally. The top-level actor's actorTimeMs is
    // threaded down so Attachment/PE1 children pause when their ancestor
    // pauses (playbackSpeed=0).
    std::vector<u32> tops;
    tops.reserve(rs_.Scene().Actors().All().size());
    for (auto& [h, mi] : rs_.Scene().Actors().All()) {
        if (!mi->IsChild())
            tops.push_back(h);
    }
    // Same reason as UpdateAttachments: this walk reaches SpawnChild through
    // the PE1/SPN paths, so handle assignment must not follow map order.
    std::sort(tops.begin(), tops.end());
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
                                       i32 ancestorClock,
                                       std::span<const Matrix44f> parentBones) {
    if (actor.IsChild() && actor.parentVisibility <= 0.02f) {
        // Hidden subtree: still tell descendants' corn fx emitters they're
        // invisible so they don't keep playing on stale state. Other
        // per-frame work (animation eval, particle sim, ribbon sim) is
        // already gated elsewhere on parentVisibility.
        SilenceCornEmittersRec(actor);
        return;
    }

    // Hoisted out of the block below so the recursion can hand it down: a
    // Skinned child is posed from these, and they are the only copy.
    FrameState fs;
    if (actor.role != ActorRole::External && actor.animation.HasSource()) {
        i32 localTimeMs;
        i32 globalTimeMs;
        i32 childElapsedMs = 0;
        const i32 seqIdx = actor.animation.ActiveSequenceIndex();

        if (actor.role == ActorRole::Unit) {
            // Top-level: trust the actor's own clock (set by Actor::Advance).
            localTimeMs = actor.animation.TimeMs();
            globalTimeMs = ctx.sceneAnimationTimeMs - actor.animation.BirthTimeMs();
        } else {
            // Child: derive local time from a clock chosen by role.
            //   - Attachment / PE1: use ancestor's actorTimeMs so paused
            //     parents pause their visual children's animation cursors.
            //   - SPN: keep using the wall clock — SPN children have a
            //     wall-clock expiry that the spawner manages, and decoupling
            //     the cursor from that expiry would let visuals run past the
            //     scheduled despawn time.
            const i32 clock =
                (actor.role == ActorRole::SPN) ? ctx.sceneAnimationTimeMs : ancestorClock;
            i32 localTime = clock - actor.animation.BirthTimeMs();
            if (localTime < 0)
                localTime = 0;
            const i32 unwrappedTime = localTime;
            const auto seqs = actor.animation.Sequences();
            if (!seqs.empty()) {
                const i32 boundedSeq = seqIdx % (i32)seqs.size();
                const auto& seq = seqs[boundedSeq];
                // Shares the playlist's windowing rather than repeating it.
                // The zero-duration case stays outside on purpose: a child
                // holds its derived clock there, where a top-level actor
                // snaps to the sequence start.
                if (seq.endMs - seq.startMs > 0)
                    localTime =
                        animation::WindowSequence(seq, localTime, actor.ignoreNonLooping).frameMs;
            }
            localTimeMs = localTime;
            globalTimeMs = localTime;
            // Before the window folded it: an M3 track loops on its own
            // duration, so a child handed only the windowed time aliases every
            // track shorter or longer than its sequence.
            childElapsedMs = unwrappedTime;
        }

        // The playlist is the authority on everything a clip carries beyond
        // sequence-and-time, and this is the path every renderer-driven actor
        // takes — `Actor::EvaluateAndApply` only runs for host-driven ones.
        // Synthesising the clip here and leaving `elapsedMs` at its default
        // pinned every M3 layer to t=0: the model rendered, posed, and never
        // moved, with a healthy draw count and a stable trace the whole time.
        //
        // `sequence` and `timeMs` are still the ticker's own values in the
        // single-play case rather than the playlist's, so Warcraft III and WoW
        // stay byte-identical across this: the two agree by construction
        // (`TimeMs()` *is* the primary play's frame time), and taking the
        // playlist's copy would swap a raw host index for a bounded one.
        const auto playlistClips = actor.animation.Playlist().Clips();
        ClipRef clip{.sequence = seqIdx, .timeMs = localTimeMs};
        if (!playlistClips.empty()) {
            const ClipRef& p = playlistClips.front();
            clip.elapsedMs = p.elapsedMs;
            clip.weight = p.weight;
            clip.speed = p.speed;
            clip.loop = p.loop;
            clip.mask = p.mask;
            clip.rootNode = p.rootNode;
            clip.subtrack = p.subtrack;
        } else {
            // Children derive their cursor from an ancestor clock and never
            // run a playlist, so the unwrapped elapsed is the pre-window time.
            clip.elapsedMs = childElapsedMs;
        }
        PoseRequest req = PoseRequest::OneClip(clip);
        // More than one play only exists once a host asks for a layer through
        // `ActorView::Play`, so the single-clip path above stays the one every
        // existing caller takes.
        if (playlistClips.size() > 1)
            req.clips = playlistClips;
        req.globalTimeMs = globalTimeMs;
        req.world = actor.ScaledWorldTransform();
        req.cameraPos = ctx.camPos;
        req.view = ctx.view;

        // A Skinned child does not pose itself: it rides the parent's rig, so
        // every bone the two share by key bone arrives already in the parent's
        // model space and the sampler is told to leave it alone. Bones the
        // pairing missed still sample and compose normally, which is what an
        // intermediate link in the chain needs.
        // Last frame's stage claims, if any. Merged with the Skinned-child
        // overrides rather than replacing them: a claimed bone and a ridden
        // bone are different sources of the same instruction, and a model can
        // in principle have both.
        std::vector<NodeOverride> ridden = actor.stageOverrides;
        if (actor.role == ActorRole::Skinned && !parentBones.empty()) {
            ridden.reserve(actor.skinnedParentBone.size());
            for (usize i = 0; i < actor.skinnedParentBone.size(); ++i) {
                const i32 from = actor.skinnedParentBone[i];
                if (from < 0 || static_cast<usize>(from) >= parentBones.size())
                    continue;
                ridden.push_back({static_cast<i32>(i), parentBones[static_cast<usize>(from)],
                                  /*replace=*/true});
            }
        }
        if (!ridden.empty())
            req.overrides = std::span<const NodeOverride>(ridden);

        fs = actor.animation.Source()->Evaluate(req);

        // Post-sampling corrections, between Evaluate and ApplyFrameState so
        // the single palette build downstream already sees them (design
        // §7.4.2 — SC2 builds twice because its stage order forces it; we do
        // not replicate that).
        //
        // In creator order, never sorted: solvers correct the animated pose,
        // and a future physics stage consumes the corrected one.
        //
        // `poseStagesEnabled` gates only the stages that need something from
        // the host. It is off by default because the viewer has no terrain for
        // IK and no aim target for a turret — a reason that does not reach a
        // `.phys` ragdoll, which is fully described by the model file. Gating
        // both on one flag is what left WoW cloth inert behind a checkbox
        // labelled "M3 pose solvers" (see `IPoseStage::NeedsHostInputs`).
        if (!actor.animation.PoseStages().empty()) {
            animation::PoseStageContext sctx;
            sctx.nodeParents = actor.render.nodeParents;
            sctx.frameDtMs = ctx.frameDtMs;
            sctx.substepPhysics = ctx.substepPhysics;
            sctx.world = actor.ScaledWorldTransform();
            sctx.queryGround = ctx.queryGround;
            sctx.aimTarget = actor.aimTarget;
            actor.stageOverrides.clear();
            for (auto& stage : actor.animation.PoseStages()) {
                if (stage->NeedsHostInputs() && !ctx.poseStagesEnabled)
                    continue;
                stage->Run(fs, sctx);
                // Collected after each stage rather than after all of them, so
                // the order a later stage sees is the order they ran in.
                for (const auto& claim : stage->Claims())
                    actor.stageOverrides.push_back({claim.node, claim.transform,
                                                    /*replace=*/true});
            }
        } else if (!actor.stageOverrides.empty()) {
            // Stages turned off mid-run: drop the claims with them, or the
            // sampler keeps skipping bones nothing is driving any more.
            actor.stageOverrides.clear();
        }

        actor.ApplyFrameState(fs, localTimeMs, ctx);
    }

    // Recurse — copy the list because ApplyFrameState may have mutated
    // children's state (worldTransform, parentVisibility) but cannot add or
    // remove children mid-walk in the current model. The copy guards against
    // any future change to that contract.
    auto childList = actor.children;
    for (u32 ch : childList) {
        if (auto* c = rs_.Scene().Actors().Find(ch)) {
            // A Skinned child shares its parent's placement exactly; its own
            // transform is never written by anything else.
            if (c->role == ActorRole::Skinned)
                c->worldTransform = actor.worldTransform;
            EvaluateActorTreeRec(*c, ctx, ancestorClock, fs.boneWorldMatrices);
        }
    }
}

// Fill a single per-actor bone palette CB on Path A. Writes the
// actor's direct-node offset matrices into slots [0..nodeCount), then
// fills the SD-only group-average pseudo-slots in
// [nodeCount..actorPaletteSize). Skips slots past actorPaletteSize —
// the shader never indexes them because the load-time rewrite kept
// every vertex's boneIdx inside the populated range.
static void WriteActorBonePalette(bls::BonePaletteCb& out, const animation::SkinningSystem& sk) {
    const i32 nodeCount = sk.NodeCount();
    const Matrix44f* offsets = sk.OffsetMatrices();
    for (i32 i = 0; i < nodeCount; ++i) {
        bls::PackBone(out.bones[i], offsets[i]);
    }
    for (const auto& group : sk.GlobalGroupAverages()) {
        if (group.globalPseudoSlot < 0 || group.globalPseudoSlot >= bls::kMaxBones)
            continue;

        // Arithmetic mean of the contributing nodes' offset matrices,
        // identical to SkinningSystem::AverageOffsetMatrices but
        // inlined here so we don't need a public accessor for it.
        Matrix44f avg = Matrix44f::zero();
        i32 contributors = 0;
        for (i32 nodeIdx : group.nodeIndices) {
            if (nodeIdx < 0 || nodeIdx >= nodeCount)
                continue;
            avg += offsets[nodeIdx];
            ++contributors;
        }
        if (contributors > 0)
            avg *= 1.0f / static_cast<f32>(contributors);
        else
            avg = Matrix44f::identity();
        bls::PackBone(out.bones[group.globalPseudoSlot], avg);
    }
}

void FrameTicker::UpdateAnimation() {
    // Plots: surface live actor + skinned-actor counts on Tracy's plot
    // panel so the user can correlate spikes with population size.
#if defined(TRACY_ENABLE)
    const i64 totalActors = static_cast<i64>(rs_.Scene().Actors().All().size());
    TracyPlot("Actors.total", totalActors);
#endif
    i64 skinnedActors = 0;
    i64 paletteWrites = 0;

    for (auto& [h, miPtr] : rs_.Scene().Actors().All()) {
        auto* mi = miPtr.get();
        if (!mi->render.skinning.HasSkeleton() || !mi->render.skinning.IsReady())
            continue;
        if (mi->parentVisibility <= 0.02f)
            continue;
        ++skinnedActors;

        {
            WDX_CPU_ZONE("ComputeOffsetMatrices");
            mi->render.skinning.ComputeOffsetMatrices();
        }

        gfx::IGFXDevice* gfx = rs_.Pipeline().Gfx();

        if (mi->render.skinning.UsesPerActorPalette()) {
            // Path A: one CB write per actor.
            const gfx::BufferHandle cb = mi->render.skinning.ActorPaletteCb();
            if (cb == gfx::BufferHandle::Invalid)
                continue;
            WDX_CPU_ZONE("Actor.BonePalette");
            ++paletteWrites;
            void* mapped = nullptr;
            {
                WDX_CPU_ZONE("MapBuffer");
                mapped = gfx->MapBuffer(cb);
            }
            if (mapped) {
                auto* bp = static_cast<bls::BonePaletteCb*>(mapped);
                {
                    WDX_CPU_ZONE("WriteActorBonePalette");
                    WriteActorBonePalette(*bp, mi->render.skinning);
                }
                {
                    WDX_CPU_ZONE("UnmapBuffer");
                    gfx->UnmapBuffer(cb);
                }
            }
            continue;
        }

        // Path B fallback: one CB per geoset, original code path, plus the
        // same palettes back to back in the HD programs' bone buffer.
        const gfx::BufferHandle boneBuffer = mi->render.skinning.BoneBuffer();
        auto* bufferBones = boneBuffer != gfx::BufferHandle::Invalid
                                ? static_cast<bls::ShaderBone*>(gfx->MapBuffer(boneBuffer))
                                : nullptr;
        for (auto& geo : mi->render.gpuGeosets) {
            if (geo.bonePaletteCb == gfx::BufferHandle::Invalid)
                continue;
            WDX_CPU_ZONE("Geoset.BonePalette");
            ++paletteWrites;
            void* mapped = nullptr;
            {
                WDX_CPU_ZONE("MapBuffer");
                mapped = gfx->MapBuffer(geo.bonePaletteCb);
            }
            if (mapped) {
                auto* bp = static_cast<bls::BonePaletteCb*>(mapped);
                constexpr i32 kSlots = bls::kMaxBones;
                static thread_local Matrix44f staging[kSlots];
                i32 realBones = 0;
                {
                    WDX_CPU_ZONE("ComputeGeosetPalette");
                    realBones =
                        mi->render.skinning.ComputeGeosetPalette(geo.geosetId, staging, kSlots);
                }
                {
                    WDX_CPU_ZONE("BuildBonePalette");
                    bls::BuildBonePalette(*bp, staging, realBones);
                }
                {
                    WDX_CPU_ZONE("UnmapBuffer");
                    gfx->UnmapBuffer(geo.bonePaletteCb);
                }
                const i32 base = mi->render.skinning.BoneBufferBase(geo.geosetId);
                // A mapped buffer's previous contents are undefined, so every
                // geoset's range is rewritten each frame, identity included.
                if (bufferBones && base >= 0) {
                    const i32 slots = std::min(
                        {realBones, mi->render.skinning.GeosetPaletteSize(geo.geosetId), kSlots});
                    for (i32 i = 0; i < slots; ++i)
                        bls::PackBone(bufferBones[base + i], staging[i]);
                    if (slots <= 0)
                        bls::PackBone(bufferBones[base], Matrix44f::identity());
                }
            }
        }
        if (bufferBones)
            gfx->UnmapBuffer(boneBuffer);
    }

#if defined(TRACY_ENABLE)
    TracyPlot("Actors.skinned", skinnedActors);
    TracyPlot("BonePalette.writes", paletteWrites);
#endif
}

void FrameTicker::UpdateParticles(f32 dt) {
    rs_.Particles().Simulate(dt);
    rs_.Splats().Tick(dt);
    // CornFx deliberately is NOT ticked here — the corn-fx service ticks
    // every emitter (CPU sim) and emits one consolidated batch of GPU
    // draws from inside its SimulateAndRender pass, which must run
    // inside an active render pass. RenderPipeline calls
    // CornEffects().SimulateAndRender(dt); we just stash dt for it.
    rs_.CornEffects().SetPendingDt(dt);
}

void FrameTicker::DriveChildModels() {
    // The sim already ran inside ParticleService::Simulate (UpdateParticles).
    // What is left here is purely the actor-tree half: resolve the child
    // template, enforce the depth / instance caps, and spawn, drive or destroy.
    // All of that has to stay at this layer — the particle service has no
    // business knowing about actors, and calling DestroyActor from inside it
    // would re-enter its own mutex through RemoveModel.
    std::vector<particle::ChildModelEvent> events;
    rs_.Particles().DrainChildModelEvents(events);

    std::vector<u32> toRemove;
    for (const auto& ev : events) {
        switch (ev.kind) {
        case particle::ChildModelEvent::Kind::Birth: {
            auto* owner = rs_.Scene().Actors().Find(ev.owner);
            if (!owner || owner->treeDepth >= model::kMaxChildModelDepth)
                break;
            if (rs_.Scene().PE1InstanceCount() >= model::kMaxChildModelInstances)
                break;

            auto* em = rs_.Particles().GetEmitter(ev.owner, particle::ParticleOutput::ChildModel,
                                                  ev.emitterId);
            if (!em)
                break;

#if WDX_ENABLE_D3
            // A Diablo III system whose particles ARE models names an `.acr` by
            // SNO id, which the child-template cache cannot build: that cache is
            // keyed on a path and always produces an MdxModelAdapter.
            if (auto* d3em = dynamic_cast<particle::d3::Emitter*>(em)) {
                const i32 sno = d3em->D3Desc().snoActor;
                if (sno < 0)
                    break;
                if (auto* child = rs_.Loader().SpawnD3ParticleActor(*owner, sno, ev.transform,
                                                                    ev.childHandle))
                    child->spawnEmitterId = ev.emitterId;
                break;
            }
#endif

            // A StarCraft II `PAR_` names a TABLE, and the pending walk drew
            // the row this particle became (RE §16.16). The loader resolves an
            // `.m3` by path, as it resolves an `.m2` model particle below.
            if (em->Desc().family == particle::EmitterDesc::Family::Sc2) {
                const auto& paths = em->Desc().childModelPaths;
                if (ev.pathIndex >= paths.size())
                    break;
                if (auto* child = rs_.Loader().SpawnModelParticle(*owner, paths[ev.pathIndex],
                                                                  ev.transform, ev.childHandle))
                    child->spawnEmitterId = ev.emitterId;
                break;
            }

            const std::string& path = em->Desc().ChildModelPath();
            if (path.empty())
                break;

            // An M2 model particle names an `.m2` (by fileDataID, in practice),
            // which the child-TEMPLATE cache cannot build — it is keyed on a
            // path and always produces an MdxModelAdapter. The loader owns that
            // route and its own per-model cache.
            if (dynamic_cast<particle::ModelParticleEmitter*>(em)) {
                if (auto* child = rs_.Loader().SpawnModelParticle(*owner, path, ev.transform,
                                                                  ev.childHandle))
                    child->spawnEmitterId = ev.emitterId;
                break;
            }

            // PreloadChildTemplates already Acquired a slot per unique child
            // path at stage time and holds it for the actor's lifetime; this
            // just resolves it. A birth that lands before the host pump has
            // parsed the MDX is dropped and retried on the next emit.
            const auto slot = rs_.Assets().Acquire(assets::AssetKind::Model, assets::kSoleSubKind, path);
            auto tmpl = rs_.Assets().ChildModelOf(slot);
            rs_.Assets().Release(slot);
            if (!tmpl)
                break;

            if (auto* child = rs_.Loader().SpawnChild(*owner, ActorRole::PE1, tmpl, ev.transform,
                                                      ev.childHandle))
                child->spawnEmitterId = ev.emitterId;
            break;
        }
        case particle::ChildModelEvent::Kind::Transform:
            if (auto* c = rs_.Scene().Actors().Find(ev.childHandle)) {
                c->worldTransform = ev.transform;
                c->parentVisibility = ev.visibility;
            }
            break;
        case particle::ChildModelEvent::Kind::Death:
            toRemove.push_back(ev.childHandle);
            break;
        }
    }

    // Tolerates handles already reaped by DestroyActor's recursion when the
    // owning actor went away mid-frame.
    for (u32 rh : toRemove)
        rs_.Loader().DestroyActor(rh);

    rs_.Spn().Tick(rs_.Scene().GetAnimationTime());
}

void FrameTicker::UpdateRibbons(f32 dt) {
    // Each ribbon emitter owns its own RNG-free edge history, so the sim is
    // order-independent here; the ordering that matters is BuildGeometry's,
    // fixed by making RibbonService's emitter map ordered.
    //
    // Driven per actor rather than with one RibbonService::Simulate call
    // because the visibility gate reads actor state, which the service has no
    // business knowing about.
    for (auto& [h, mi] : rs_.Scene().Actors().All()) {
        if (mi->parentVisibility <= 0.02f)
            continue;
        rs_.Ribbons().SimulateModel(h, dt);
    }
}

} // namespace whiteout::flakes::renderer
