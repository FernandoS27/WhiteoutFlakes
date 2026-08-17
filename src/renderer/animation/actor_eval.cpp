// ============================================================================
// Actor evaluation — lifted out of RenderService.
//
// EvaluateAndApply / ApplyFrameState are conceptually per-actor: an actor owns
// its animation source, its bone matrices, and its event/particle/ribbon/
// attachment state. The renderer-wide context (camera position, scene clock,
// reference services) flows in through ActorEvalContext rather than a
// back-pointer to RenderService. Multiple actors with independent animations
// can be evaluated in any order; the caller is responsible for serializing
// against rendering.
// ============================================================================

#include "animation/actor_eval_context.h"
#include "constants.h"
#include "corn_effects/corn_effects_emitter.h"
#include "corn_effects/corn_effects_service.h"
#include "effects/spn_spawner.h"
#include "model/model_instance.h"
#include "model/model_template.h"
#include "particle/child_model_emitter.h"
#include "particle/particle_service.h"
#include "particle/rnd_seed.h"
#include "particle/splat_service.h"
#include "ribbon/ribbon_service.h"
#include "scene_manager.h"
#include "whiteout/flakes/model_types.h"
#include "whiteout/flakes/sound_emitter.h"
#include "whiteout/flakes/types.h"
#include "whiteout/flakes/util/coordinate_system.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace whiteout::flakes::renderer::model {

using namespace ::whiteout::flakes::renderer::animation;
using namespace ::whiteout::flakes::renderer::effects;

namespace {

// Upload the actor's world-space bone matrices into its skinning palette.
// (Billboarding is applied in MdxHierarchy::Evaluate so it propagates to children.)
void ApplyBoneMatrices(Actor& mi, const FrameState& state) {
    if (state.boneWorldMatrices.empty())
        return;

    const i32 bc = static_cast<i32>(state.boneWorldMatrices.size());

    // Billboarding (including inheritance through billboarded helpers) is now
    // applied inside the hierarchy traversal (MdxHierarchy::Evaluate), so these
    // bone matrices already face the camera where needed — just upload them.
    std::vector<f32> worldFlat(bc * 16);
    for (i32 i = 0; i < bc; i++)
        std::memcpy(&worldFlat[i * 16], &state.boneWorldMatrices[i].data[0][0], 64);
    mi.render.skinning.UpdateNodeMatrices(bc, worldFlat.data());
}

void ApplyRibbonFrameStates(Actor& mi, const FrameState& state, ribbon::RibbonService& ribbons) {
    for (const auto& rs : state.ribbonStates) {
        ribbon::RibbonState st;
        st.transform = rs.transform;
        st.above = rs.above;
        st.below = rs.below;
        st.alpha = rs.alpha;
        st.color = rs.color;
        st.visibility = rs.visibility;
        st.slot = rs.slot;
        ribbons.SetState(mi.handle, rs.emitterId, st);
    }
}

void ApplyParticleFrameStates(Actor& mi, const FrameState& state,
                              particle::ParticleService& particles, const Vector3f& camPos) {
    // WoW fades an emitter's rate with distance, measured once per MODEL from
    // its world transform — not per emitter from the bone the emitter rides.
    // Every dialect but WoW's ignores the value entirely.
    //
    // In the emitter's OWN units, which for a `.m2` is yards: the falloff's
    // constants (a 50-yard knee, a 0.25 floor) are calibrated against gameplay
    // distances, and the transform above has already scaled the model into
    // renderer units by WorldScale — 100 for WoW. Feeding it the unscaled
    // number puts every emitter past the knee and pins the whole scene at the
    // floor, which looks like a working falloff and is a quarter-rate bug.
    const Vector3f origin = whiteout::transform_point({0, 0, 0}, mi.ScaledWorldTransform());
    const f32 dx = origin.x - camPos.x, dy = origin.y - camPos.y, dz = origin.z - camPos.z;
    const f32 distSq = dx * dx + dy * dy + dz * dz;
    const f32 scale = (mi.worldScale > 0.0f) ? mi.worldScale : 1.0f;
    const f32 viewDist = (distSq > 0.0f) ? (std::sqrt(distSq) / scale) : 0.0f;

    for (usize i = 0; i < state.particleStates.size(); ++i) {
        const auto& ps = state.particleStates[i];
        // A model-particle emitter is registered under ChildModel, but its
        // animated state is an ordinary ParticleFrameState — the sim does not
        // care what the output is, so this loop drives both id spaces.
        auto* em = particles.GetEmitter(mi.handle,
                                       ps.modelParticle ? particle::ParticleOutput::ChildModel
                                                        : particle::ParticleOutput::Billboard,
                                       ps.emitterId);
        if (!em)
            continue;

        // Everything the emitter itself needs — rate, spawn params, gravity,
        // visibility, transform — goes through one virtual call, so this loop
        // no longer has to know which kind of emitter it is driving.
        em->ApplyState(ps);
        em->SetViewDistance(viewDist);
        // A bone generator spawns off the skeleton, so it needs the model's
        // table rather than an area of its own. Pushed here because the table
        // is per model and the emitter has no route to the FrameState.
        if (ps.boneGenerator)
            em->SetBoneSpawnTable(state.boneSpawnTable);

        // The squirt edge (rate crossing zero) is actor state, not emitter
        // state: it needs last frame's rate, which lives on the actor.
        if (ps.squirting) {
            auto& st = mi.render.pe2State[i];
            if (st.emissionValid) {
                if (ps.emissionRate > 0.02f && st.lastEmissionRate <= 0.02f)
                    em->SetSquirtPending(true);
            }
            st.lastEmissionRate = ps.emissionRate;
            st.emissionValid = true;
        }
    }
}

// PE1 emitters live in the same service; only the frame-state struct differs.
void ApplyChildModelFrameStates(Actor& mi, const FrameState& state,
                                particle::ParticleService& particles) {
    for (const auto& ps : state.pe1States) {
        auto* em = particles.GetEmitter(mi.handle, particle::ParticleOutput::ChildModel,
                                        ps.emitterId);
        if (!em)
            continue;
        static_cast<particle::ChildModelEmitter*>(em)->ApplyPE1State(ps);
    }
}

void ApplyCornFrameStates(Actor& mi, const FrameState& state, const ActorEvalContext& ctx) {
    if (!ctx.cornEffects)
        return;

    // Resolve the active sequence name once per actor so each emitter can
    // re-evaluate its `animVisibilityGuide` against it. Mirrors the engine's
    // CParticleEmitter (corn fx)::SetCurrentAnimationName call site.
    // `Sequences()` returns by VALUE — keep the std::string alive across
    // the loop or the .c_str() pointers go invalid.
    std::string curAnimName;
    bool forcedLoopNonLooping = false;
    {
        const i32 sidx = mi.animation.ActiveSequenceIndex();
        const auto seqs = mi.animation.Sequences();
        if (sidx >= 0 && sidx < (i32)seqs.size()) {
            curAnimName = seqs[sidx].name;
            forcedLoopNonLooping = mi.ignoreNonLooping && seqs[sidx].nonLooping;
        }
    }

    // Per-actor team color — Actor::teamColor is packed 0x00BBGGRR with
    // alpha implicit 0xFF (matches Actor::SetTeamColor's encoding). Each
    // corn fx emitter owns the color it pushes to cornflakes via the
    // Game.TeamColor attribute, so every actor's emitters get THIS
    // actor's swatch even when two actors of the same MDX coexist.
    const Vector4f teamRGBA = {
        ((mi.teamColor) & 0xFF) / 255.0f,
        ((mi.teamColor >> 8) & 0xFF) / 255.0f,
        ((mi.teamColor >> 16) & 0xFF) / 255.0f,
        1.0f,
    };

    // Effective owning-agent visibility: the actor's own parentVisibility
    // (carries the attachment-slot signal) AND the visibility of every
    // ancestor up to the root (PE1 / SPN / Attachment children inherit the
    // hide signal of the unit they belong to, even when the immediate
    // parentVisibility field on the child has never been written by an
    // attachment driver). cs.visibility carries the per-emitter bone-chain
    // gate computed by gateByBoneAncestors in the MDX evaluator.
    bool ancestorVisible = mi.parentVisibility > 0.0f;
    if (ctx.scene) {
        for (u32 ph = mi.parent; ph != 0 && ancestorVisible;) {
            auto* p = ctx.scene->Actors().Find(ph);
            if (!p)
                break;
            if (p->parentVisibility <= 0.0f) {
                ancestorVisible = false;
                break;
            }
            ph = p->parent;
        }
    }

    for (const auto& cs : state.cornStates) {
        auto* em = ctx.cornEffects->GetEmitter(mi.handle, cs.emitterId);
        if (!em)
            continue;
        em->SetCurrentAnimationName(curAnimName.c_str());
        if (forcedLoopNonLooping && em->IsNonLoopingEffect())
            em->SyncSequenceCycle(mi.animation.Playlist().SequenceCycle());
        em->SetReplaceableColor(teamRGBA);
        em->SetModelToWorld(cs.transform);
        em->SetScale(cs.scale);
        em->SetEmissionRateMultiplier(cs.emissionRateMul);
        em->SetLifeSpanMultiplier(cs.lifeSpanMul);
        em->SetSpeedMultiplier(cs.speedMul);
        em->SetColor(cs.color);
        const bool nodeVisible = cs.visibility > 0.0f;
        em->SetOwningAgentVisibility(ancestorVisible && nodeVisible);
    }
}

void ApplyAttachmentStates(Actor& mi, const FrameState& state, const ActorEvalContext& ctx) {
    if (!ctx.scene)
        return;
    const i32 ancestorClock = AncestorActorTimeMs(mi, ctx.scene->Actors());
    for (auto& as : state.attachmentStates) {
        if (as.attachmentIndex < 0 || as.attachmentIndex >= (i32)mi.attachmentSlots.size())
            continue;
        auto& slot = mi.attachmentSlots[as.attachmentIndex];
        if (slot.childModelHandle == 0)
            continue;
        auto* child = ctx.scene->Actors().Find(slot.childModelHandle);
        if (!child)
            continue;

        const bool visible = (as.visibility > 0.0f);
        child->worldTransform = as.transform;

        if (visible && !slot.wasVisible) {
            child->animation.SetBirthTimeMs(ancestorClock);
            auto seqs = child->animation.Sequences();
            if (!seqs.empty()) {
                // Was libc `rand()` — never seeded, state shared process-wide,
                // so the sequence a re-shown attachment picks depended on how
                // many other draws had consumed the global stream. MixSeed is
                // the particle sim's deterministic mixer; keying it on the
                // parent handle and the slot makes the choice a function of
                // the scene.
                const u32 pick = particle::MixSeed(mi.handle, (u32)as.attachmentIndex);
                child->animation.SetActiveSequenceIndex((i32)(pick % (u32)seqs.size()));
            }
            slot.wasVisible = true;
        } else if (!visible) {
            slot.wasVisible = false;
        }

        child->parentVisibility = visible ? 1.0f : 0.0f;
        // SetReverseCulling recurses into the child list, so an attachment
        // inherits its parent's winding rather than keeping its own.
        child->mirrored = mi.mirrored;
    }
}

} // namespace

void Actor::ApplyFrameState(const FrameState& state, i32 localTimeMs, const ActorEvalContext& ctx) {
    ApplyBoneMatrices(*this, state);
    render.ApplyGeosetStates(state);
    render.ApplyLayerStates(state);
    if (ctx.particles)
        ApplyParticleFrameStates(*this, state, *ctx.particles, ctx.camPos);
    if (ctx.ribbons)
        ApplyRibbonFrameStates(*this, state, *ctx.ribbons);
    if (ctx.particles)
        ApplyChildModelFrameStates(*this, state, *ctx.particles);

    for (i32 i = 0;
         i < (i32)state.collisionTransforms.size() && i < (i32)render.collisionShapes.size(); i++)
        render.collisionShapes[i].transform = state.collisionTransforms[i];

    ApplyAttachmentStates(*this, state, ctx);
    ApplyCornFrameStates(*this, state, ctx);

    if (ctx.fireEvents && !events.Empty()) {
        const i32 activeSeq = animation.ActiveSequenceIndex();

        i32 seqStart = 0, seqEnd = 0x7FFFFFFF;
        if (animation.Source()) {
            auto seqs = animation.Source()->GetSequences();
            if (activeSeq >= 0 && activeSeq < (i32)seqs.size()) {
                seqStart = seqs[activeSeq].startMs;
                seqEnd = seqs[activeSeq].endMs;
            }
        }
        events.Tick(*this, state.boneWorldMatrices, activeSeq, localTimeMs,
                    ctx.sceneAnimationTimeMs, seqStart, seqEnd, ctx.splats, ctx.spnSpawner,
                    ctx.sound);
    }
}

void Actor::Advance(f32 dtSec) {
    if (!animation.HasSource())
        return;

    const i32 dtMs = (dtSec > 0.0f) ? (i32)(dtSec * playbackSpeed * 1000.0f + 0.5f) : 0;
    cursor.actorTimeMs += dtMs;
    animation.Advance(cursor.actorTimeMs, ignoreNonLooping);
}

void Actor::EvaluateAndApply(const ActorEvalContext& ctx) {
    if (!animation.HasSource())
        return;
    const i32 globalTime = ctx.sceneAnimationTimeMs - animation.BirthTimeMs();
    const i32 localTime = animation.TimeMs();

    // The whole stack, not just the primary play: a single-clip adapter reads
    // PrimaryClip() and is unaffected, while a layering one sees every play.
    //
    // This is the HOST-driven path only — the Max plugin's timeline scrub and
    // anything calling `ActorView::EvaluateAndApply`. Renderer-driven actors go
    // through `FrameTicker::EvaluateActorTreeRec`, which builds its own clip
    // because a child derives its cursor from an ancestor clock rather than
    // from a playlist. Any change to what a clip must carry has to land in both
    // places: fixing only this one is what left `.m3` rendering in bind pose.
    const auto clips = animation.Playlist().Clips();
    const ClipRef fallback{.sequence = animation.ActiveSequenceIndex(), .timeMs = localTime};
    PoseRequest req;
    req.clips = clips.empty() ? std::span<const ClipRef>(&fallback, 1) : clips;
    req.globalTimeMs = globalTime;
    req.world = ScaledWorldTransform();
    req.cameraPos = ctx.camPos;
    FrameState fs = animation.Source()->Evaluate(req);
    ApplyFrameState(fs, localTime, ctx);
}

} // namespace whiteout::flakes::renderer::model
