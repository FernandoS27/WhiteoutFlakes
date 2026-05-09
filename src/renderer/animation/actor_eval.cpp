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
#include "whiteout/flakes/types.h"
#include "constants.h"
#include "whiteout/flakes/util/coordinate_system.h"
#include "model/model_instance.h"
#include "model/model_template.h"
#include "whiteout/flakes/model_types.h"
#include "particle/particle_service.h"
#include "particle/splat_service.h"
#include "corn_effects/corn_effects_service.h"
#include "corn_effects/corn_effects_emitter.h"
#include "scene_manager.h"
#include "whiteout/flakes/sound_emitter.h"
#include "effects/spn_spawner.h"

#include <algorithm>
#include <cstring>
#include <vector>

namespace whiteout::flakes::renderer::model {

using namespace ::whiteout::flakes::renderer::animation;
using namespace ::whiteout::flakes::renderer::effects;

namespace {

// Compute world-space bone matrices with billboard adjustments and push them
// into the actor's skinning palette.
void ApplyBoneMatrices(Actor& mi, const FrameState& state, const Vector3f& camPos) {
    if (state.boneWorldMatrices.empty()) return;

    const i32 bc = static_cast<i32>(state.boneWorldMatrices.size());

    const std::vector<u32>&      billboardFlags = mi.sourceTemplate
        ? mi.sourceTemplate->skeleton.billboardFlags : mi.render.billboardFlags;
    const std::vector<Vector3f>& nodePivots     = mi.sourceTemplate
        ? mi.sourceTemplate->skeleton.nodePivots   : mi.render.nodePivots;
    const std::vector<i32>&      nodeParents    = mi.sourceTemplate
        ? mi.sourceTemplate->skeleton.nodeParents  : mi.render.nodeParents;

    std::vector<f32> worldFlat(bc * 16);
    for (i32 i = 0; i < bc; i++) {
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

                const f32 sX = rowToVec(boneM, 0).length();
                const f32 sY = rowToVec(boneM, 1).length();
                const f32 sZ = rowToVec(boneM, 2).length();

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

        std::memcpy(&worldFlat[i * 16], &boneM.data[0][0], 64);
    }
    mi.render.skinning.UpdateNodeMatrices(bc, worldFlat.data());
}

void ApplyParticleFrameStates(Actor& mi, const FrameState& state,
                              particle::ParticleService& particles) {
    for (usize i = 0; i < state.particleStates.size(); ++i) {
        const auto& ps = state.particleStates[i];
        auto* em = particles.GetEmitter(mi.handle, ps.emitterId);
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

void ApplyCornFrameStates(Actor& mi, const FrameState& state,
                          const ActorEvalContext& ctx) {
    if (!ctx.cornEffects) return;

    // Resolve the active sequence name once per actor so each emitter can
    // re-evaluate its `animVisibilityGuide` against it. Mirrors the engine's
    // CParticleEmitter (corn fx)::SetCurrentAnimationName call site.
    // `Sequences()` returns by VALUE — keep the std::string alive across
    // the loop or the .c_str() pointers go invalid.
    std::string curAnimName;
    {
        const i32 sidx = mi.animation.ActiveSequenceIndex();
        const auto seqs = mi.animation.Sequences();
        if (sidx >= 0 && sidx < (i32)seqs.size()) curAnimName = seqs[sidx].name;
    }

    // Per-actor team color — Actor::teamColor is packed 0x00BBGGRR with
    // alpha implicit 0xFF (matches Actor::SetTeamColor's encoding). Each
    // corn fx emitter owns the color it pushes to cornflakes via the
    // Game.TeamColor attribute, so every actor's emitters get THIS
    // actor's swatch even when two actors of the same MDX coexist.
    const Vector4f teamRGBA = {
        ((mi.teamColor       ) & 0xFF) / 255.0f,
        ((mi.teamColor >>  8 ) & 0xFF) / 255.0f,
        ((mi.teamColor >> 16 ) & 0xFF) / 255.0f,
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
        for (u32 ph = mi.parent; ph != 0 && ancestorVisible; ) {
            auto* p = ctx.scene->Actors().Find(ph);
            if (!p) break;
            if (p->parentVisibility <= 0.0f) { ancestorVisible = false; break; }
            ph = p->parent;
        }
    }

    for (const auto& cs : state.cornStates) {
        auto* em = ctx.cornEffects->GetEmitter(mi.handle, cs.emitterId);
        if (!em) continue;
        em->SetCurrentAnimationName(curAnimName.c_str());
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

void ApplyAttachmentStates(Actor& mi, const FrameState& state,
                           const ActorEvalContext& ctx) {
    if (!ctx.scene) return;
    const i32 ancestorClock = AncestorActorTimeMs(mi, ctx.scene->Actors());
    for (auto& as : state.attachmentStates) {
        if (as.attachmentIndex < 0 ||
            as.attachmentIndex >= (i32)mi.attachmentSlots.size()) continue;
        auto& slot = mi.attachmentSlots[as.attachmentIndex];
        if (slot.childModelHandle == 0) continue;
        auto* child = ctx.scene->Actors().Find(slot.childModelHandle);
        if (!child) continue;

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

}  // namespace

void Actor::ApplyFrameState(const FrameState& state, i32 localTimeMs,
                            const ActorEvalContext& ctx) {
    ApplyBoneMatrices(*this, state, ctx.camPos);
    render.ApplyGeosetStates(state);
    render.ApplyLayerStates(state);
    if (ctx.particles)
        ApplyParticleFrameStates(*this, state, *ctx.particles);
    render.ApplyRibbonFrameStates(state);
    render.ApplyPE1FrameStates(state);

    for (i32 i = 0; i < (i32)state.collisionTransforms.size()
                  && i < (i32)render.collisionShapes.size(); i++)
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
                seqEnd   = seqs[activeSeq].endMs;
            }
        }
        events.Tick(*this,
                    state.boneWorldMatrices,
                    activeSeq,
                    localTimeMs, ctx.sceneAnimationTimeMs,
                    seqStart, seqEnd,
                    ctx.splats,
                    ctx.spnSpawner,
                    ctx.sound);
    }
}

void Actor::Advance(f32 dtSec) {
    if (!animation.HasSource()) return;

    const i32 dtMs = (dtSec > 0.0f) ? (i32)(dtSec * playbackSpeed * 1000.0f + 0.5f) : 0;
    cursor.actorTimeMs += dtMs;
    const i32 now = cursor.actorTimeMs;

    const auto seqs = animation.Sequences();
    if (seqs.empty()) return;

    const i32 rawIdx     = animation.ActiveSequenceIndex();
    const i32 boundedIdx = ((rawIdx % (i32)seqs.size()) + (i32)seqs.size()) % (i32)seqs.size();
    if (rawIdx != cursor.prevActiveSequence) {
        cursor.sequenceStartTimeMs = now;
        cursor.prevActiveSequence  = rawIdx;
    }

    const auto& seq      = seqs[boundedIdx];
    const i32   duration = seq.endMs - seq.startMs;
    i32 elapsed = now - cursor.sequenceStartTimeMs;
    if (elapsed < 0) elapsed = 0;

    i32 frameMs;
    if (duration <= 0) {
        frameMs = seq.startMs;
    } else if (seq.nonLooping && !ignoreNonLooping) {
        frameMs = seq.startMs + (std::min)(elapsed, duration);
    } else {
        frameMs = seq.startMs + (elapsed % duration);
    }
    animation.SetTimeMs(frameMs);
}

void Actor::EvaluateAndApply(const ActorEvalContext& ctx) {
    if (!animation.HasSource()) return;
    const i32 globalTime = ctx.sceneAnimationTimeMs - animation.BirthTimeMs();
    const i32 localTime  = animation.TimeMs();
    FrameState fs = animation.Source()->Evaluate(
        animation.ActiveSequenceIndex(), localTime, globalTime,
        worldTransform, ctx.camPos);
    ApplyFrameState(fs, localTime, ctx);
}

}  // namespace whiteout::flakes::renderer::animation
