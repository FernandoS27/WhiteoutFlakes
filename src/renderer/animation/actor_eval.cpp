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
#include "particle/particle_service.h"
#include "particle/splat_service.h"
#include "scene_manager.h"
#include "whiteout/flakes/model_types.h"
#include "whiteout/flakes/sound_emitter.h"
#include "whiteout/flakes/types.h"
#include "whiteout/flakes/util/coordinate_system.h"

#include <algorithm>
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

void ApplyParticleFrameStates(Actor& mi, const FrameState& state,
                              particle::ParticleService& particles) {
    for (usize i = 0; i < state.particleStates.size(); ++i) {
        const auto& ps = state.particleStates[i];
        auto* em = particles.GetEmitter(mi.handle, ps.emitterId);
        if (!em)
            continue;

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

        em->SetModelToWorld(CoordinateSystem::ConvertTransform(CoordinateSystem::Default(),
                                                               em->GetCoordSpace(), ps.transform));
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
            em->SyncSequenceCycle(mi.cursor.sequenceCycle);
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
            if (!seqs.empty())
                child->animation.SetActiveSequenceIndex(rand() % (i32)seqs.size());
            slot.wasVisible = true;
        } else if (!visible) {
            slot.wasVisible = false;
        }

        child->parentVisibility = visible ? 1.0f : 0.0f;
    }
}

} // namespace

void Actor::ApplyFrameState(const FrameState& state, i32 localTimeMs, const ActorEvalContext& ctx) {
    ApplyBoneMatrices(*this, state);
    render.ApplyGeosetStates(state);
    render.ApplyLayerStates(state);
    if (ctx.particles)
        ApplyParticleFrameStates(*this, state, *ctx.particles);
    render.ApplyRibbonFrameStates(state);
    render.ApplyPE1FrameStates(state);

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
    const i32 now = cursor.actorTimeMs;

    const auto seqs = animation.Sequences();
    if (seqs.empty())
        return;

    const i32 rawIdx = animation.ActiveSequenceIndex();
    const i32 boundedIdx = ((rawIdx % (i32)seqs.size()) + (i32)seqs.size()) % (i32)seqs.size();
    if (rawIdx != cursor.prevActiveSequence) {
        cursor.sequenceStartTimeMs = now;
        cursor.prevActiveSequence = rawIdx;
        ++cursor.sequenceCycle;
    }

    const auto& seq = seqs[boundedIdx];
    const i32 duration = seq.endMs - seq.startMs;
    i32 elapsed = now - cursor.sequenceStartTimeMs;
    if (elapsed < 0)
        elapsed = 0;

    i32 frameMs;
    if (duration <= 0) {
        frameMs = seq.startMs;
    } else if (seq.nonLooping && !ignoreNonLooping) {
        frameMs = seq.startMs + (std::min)(elapsed, duration);
    } else {
        // Roll sequenceStartTimeMs forward by whole durations so elapsed
        // stays in [0, duration). Each roll is a fresh cycle — consumers
        // (corn-fx single-shot effects under ignoreNonLooping) key off
        // sequenceCycle to re-fire per loop.
        if (elapsed >= duration) {
            const i32 cycles = elapsed / duration;
            cursor.sequenceStartTimeMs += cycles * duration;
            cursor.sequenceCycle += cycles;
            elapsed -= cycles * duration;
        }
        frameMs = seq.startMs + elapsed;
    }
    animation.SetTimeMs(frameMs);
}

void Actor::EvaluateAndApply(const ActorEvalContext& ctx) {
    if (!animation.HasSource())
        return;
    const i32 globalTime = ctx.sceneAnimationTimeMs - animation.BirthTimeMs();
    const i32 localTime = animation.TimeMs();
    FrameState fs = animation.Source()->Evaluate(animation.ActiveSequenceIndex(), localTime,
                                                 globalTime, worldTransform, ctx.camPos);
    ApplyFrameState(fs, localTime, ctx);
}

} // namespace whiteout::flakes::renderer::model
