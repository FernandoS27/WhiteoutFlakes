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
#if WDX_ENABLE_D3
#include "particle/d3_emitter.h"
#endif
#include "particle/particle_service.h"
#include "particle/rnd_seed.h"
#include "particle/sc2_compose.h"
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
#include <span>
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

void ApplyRibbonFrameStates(Actor& mi, const FrameState& state, ribbon::RibbonService& ribbons,
                            const ribbon::GroundQuery& queryGround) {
    for (const auto& rs : state.ribbonStates) {
        ribbon::RibbonState st;
        st.transform = rs.transform;
        // The grid (or the host's terrain) the SC2 legacy integrator collides
        // against — the same query terrain IK plants feet on.
        st.groundQuery = queryGround;
        st.above = rs.above;
        st.below = rs.below;
        st.alpha = rs.alpha;
        st.color = rs.color;
        st.visibility = rs.visibility;
        st.slot = rs.slot;
        st.unitScale = rs.unitScale;
        std::copy(std::begin(rs.texAnimRow0), std::end(rs.texAnimRow0), std::begin(st.texAnimRow0));
        std::copy(std::begin(rs.texAnimRow1), std::end(rs.texAnimRow1), std::begin(st.texAnimRow1));
        // SC2 sampled block: the .m3 evaluator fills rs.sc2; MDX/M2 leave it
        // inert, so the copy is harmless for the WC3 family.
        st.sc2.speed = rs.sc2.speed;
        st.sc2.yawDeg = rs.sc2.yawDeg;
        st.sc2.pitchDeg = rs.sc2.pitchDeg;
        st.sc2.lifetime = rs.sc2.lifetime;
        st.sc2.maxLength = rs.sc2.maxLength;
        st.sc2.size3 = rs.sc2.size3;
        std::copy(std::begin(rs.sc2.color3), std::end(rs.sc2.color3), std::begin(st.sc2.color3));
        st.sc2.rotation3 = rs.sc2.rotation3;
        st.sc2.active = rs.sc2.active;
        st.sc2.splineNodeTransform = rs.sc2.splineNodeTransform;
        st.sc2.velocityBaseFactor = rs.sc2.velocityBaseFactor;
        st.sc2.velocityEndFactor = rs.sc2.velocityEndFactor;
        st.sc2.splineYawDeg = rs.sc2.splineYawDeg;
        st.sc2.splinePitchDeg = rs.sc2.splinePitchDeg;
        st.sc2.parentVelocityScale = rs.sc2.parentVelocityScale;
        std::copy(std::begin(rs.sc2.waveAmp), std::end(rs.sc2.waveAmp), std::begin(st.sc2.waveAmp));
        std::copy(std::begin(rs.sc2.waveFreq), std::end(rs.sc2.waveFreq), std::begin(st.sc2.waveFreq));
        st.sc2.overlayPhase = rs.sc2.overlayPhase;
        std::copy(std::begin(rs.sc2.splineWaveAmp), std::end(rs.sc2.splineWaveAmp),
                  std::begin(st.sc2.splineWaveAmp));
        std::copy(std::begin(rs.sc2.splineWaveFreq), std::end(rs.sc2.splineWaveFreq),
                  std::begin(st.sc2.splineWaveFreq));
        ribbons.SetState(mi.handle, rs.emitterId, st);
    }
}

void ApplyParticleFrameStates(Actor& mi, const FrameState& state,
                              particle::ParticleService& particles, const Vector3f& camPos,
                              const Matrix44f& view, i32 frameDtMs) {
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

        if (em->Desc().family == particle::EmitterDesc::Family::Sc2) {
            // What a model particle's pose reads from the scene, and the
            // actor's world scale it runs its SC2 units against.
            em->SetSc2Scene(view, mi.worldScale);
            // Shape 7 is born on the live surface. Node matrices, not the
            // palette's offsets, for the reason the Diablo III push gives:
            // these were written by ApplyBoneMatrices at the top of this pass.
            if (em->Desc().sc2.emit.shape == static_cast<u8>(particle::Sc2SpawnShape::Mesh)) {
                const auto data = mi.render.skinning.SharedData();
                em->SetEmitMeshPose(mi.render.skinning.NodeMatrices(),
                                    data ? std::span<const Matrix44f>(data->inverseBindMatrices)
                                         : std::span<const Matrix44f>{},
                                    mi.ScaledWorldTransform());
            }
        }

        // StarCraft II's squirt keys: a burst when a playhead steps over a
        // key, which takes last frame's cursors — actor memory, like the WC3
        // squirt edge below. Every player the model is playing, not the top
        // layer alone.
        if (em->Desc().family == particle::EmitterDesc::Family::Sc2 &&
            i < mi.render.sc2ParticleClocks.size()) {
            const particle::Sc2Crossing crossing = particle::Sc2CrossSquirtKeys(
                em->Desc().sc2, state.sc2AnimPlayers, mi.render.sc2ParticleClocks[i], frameDtMs);
            for (usize s = 0; s < crossing.bursts.size(); ++s) {
                if (crossing.bursts[s] != 0)
                    em->QueueBurst(static_cast<u32>(s), crossing.bursts[s]);
            }
            // The pre-roll follows the ACTIVE sequence, which the player list
            // does not name: a global loop starting under it asks nothing.
            em->SetSc2ActiveSequence(particle::Sc2ActiveSequence(state.sc2AnimPlayers));
        }

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
        // Checked, not a static_cast: the child-model space is no longer PE1's
        // alone — a `.prt` whose particles are models registers there too, and
        // it has no PE1 state to apply. One actor cannot produce both, so this
        // never fires; it costs a type check per PE1 emitter per frame and buys
        // a reinterpret that would be silent.
        auto* pe1 = dynamic_cast<particle::ChildModelEmitter*>(em);
        if (!pe1)
            continue;
        pe1->ApplyPE1State(ps);
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

// The active sequence's [start, end] in milliseconds, or the whole timeline
// when there is no sequence to ask. Both event pools window on it.
void SequenceWindowMs(const Actor& mi, i32 activeSeq, i32& lo, i32& hi) {
    lo = 0;
    hi = 0x7FFFFFFF;
    if (!mi.animation.Source())
        return;
    auto seqs = mi.animation.Source()->GetSequences();
    if (activeSeq >= 0 && activeSeq < (i32)seqs.size()) {
        lo = seqs[activeSeq].startMs;
        hi = seqs[activeSeq].endMs;
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

#if WDX_ENABLE_D3
// Diablo III emitters take no FrameState at all. Everything animated about a
// `.prt` lives inside the file, evaluated against the system's own clock, so
// the one thing the host owes an emitter is where it is — its bone's world
// matrix, decomposed into a position and an orientation. That orientation is
// load-bearing: it is frozen onto every particle at birth and is what makes
// the emitter-local kinematic triple local.
void ApplyD3ParticleFrames(Actor& mi, const FrameState& state,
                           particle::ParticleService& particles,
                           const ActorEvalContext& ctx) {
    particles.ForEachEmitter([&](const particle::EmitterKey& key, const particle::Emitter2& e) {
        if (key.model != mi.handle)
            return;
        auto* d3 = dynamic_cast<particle::d3::Emitter*>(const_cast<particle::Emitter2*>(&e));
        if (!d3)
            return;
        const i32 bone = d3->AttachBone();
        Matrix44f m = mi.ScaledWorldTransform();
        if (bone >= 0 && bone < static_cast<i32>(state.boneWorldMatrices.size()))
            m = state.boneWorldMatrices[bone] * m;
        // The hardpoint's own frame, inside the bone. Identity unless the
        // event named a hardpoint, so a bone-attached or origin-attached
        // emitter costs nothing here.
        m = d3->AttachOffset() * m;
        d3->SetModelToWorld(m);
        d3->SetWorldPosition(whiteout::transform_point({0, 0, 0}, m));
        d3->SetVisible(true);
        // Renderer units per Diablo III unit. The matrix above already carries
        // it, so the emitter's POSITION arrives scaled while every size, extent
        // and speed inside the `.prt` is still raw — and the two are added
        // together. Without this a particle draws at 1/17th of its authored
        // size and its whole flight path shrinks to a clump on the bone.
        d3->SetUnitScale(mi.worldScale);
        // Render modes 9 and 10 conform their quads to the ground, through the
        // query the scene's particle service installs on every emitter it holds
        // — once, and again only when the host replaces it — so it is not
        // copied into the emitter here every frame.
        // Render modes 0 and 13 turn a spawned CHILD ACTOR to the camera, and
        // that decision is made at emit rather than at draw, so the emitter has
        // to be holding the view direction before it emits.
        d3->SetCameraForward(
            {-ctx.view.data[0][2], -ctx.view.data[1][2], -ctx.view.data[2][2]});
        // The live surface, for the three mesh emitter shapes. Node matrices
        // rather than the palette's offset matrices: those are recomputed on
        // the draw path and would be a frame stale here, while these were
        // written by ApplyBoneMatrices at the top of this same pass.
        if (d3->HasEmitMesh()) {
            const auto data = mi.render.skinning.SharedData();
            d3->SetEmitMeshPose(mi.render.skinning.NodeMatrices(),
                                data ? std::span<const Matrix44f>(data->inverseBindMatrices)
                                     : std::span<const Matrix44f>{},
                                mi.ScaledWorldTransform());
        }
        // The rotation part of the bone matrix, orthonormalised. A scaled bone
        // would otherwise hand the birth quaternion a scale, and a quaternion
        // has nowhere to put one.
        Vector3f x{m.data[0][0], m.data[0][1], m.data[0][2]};
        Vector3f y{m.data[1][0], m.data[1][1], m.data[1][2]};
        Vector3f z{m.data[2][0], m.data[2][1], m.data[2][2]};
        x.normalize();
        y.normalize();
        z.normalize();
        const f32 tr = x.x + y.y + z.z;
        Quaternion q = Quaternion::identity();
        if (tr > 0.0f) {
            const f32 s = std::sqrt(tr + 1.0f) * 2.0f;
            q = {(y.z - z.y) / s, (z.x - x.z) / s, (x.y - y.x) / s, 0.25f * s};
        } else if (x.x > y.y && x.x > z.z) {
            const f32 s = std::sqrt(1.0f + x.x - y.y - z.z) * 2.0f;
            q = {0.25f * s, (y.x + x.y) / s, (z.x + x.z) / s, (y.z - z.y) / s};
        } else if (y.y > z.z) {
            const f32 s = std::sqrt(1.0f + y.y - x.x - z.z) * 2.0f;
            q = {(y.x + x.y) / s, 0.25f * s, (z.y + y.z) / s, (z.x - x.z) / s};
        } else {
            const f32 s = std::sqrt(1.0f + z.z - x.x - y.y) * 2.0f;
            q = {(z.x + x.z) / s, (z.y + y.z) / s, 0.25f * s, (x.y - y.x) / s};
        }
        q.normalize();
        d3->SetEmitterOrientation(q);
    });
}

// A Diablo III child model rides a HARDPOINT of its parent, and there is no
// attachment node and no per-frame track behind it — a TriggerEvent named the
// child and stamped the placement on it at spawn. So this is the whole of the
// per-frame work; the arithmetic is in BoneRidingTransform.
void ApplyD3AttachedChildren(Actor& mi, const FrameState& state, const ActorEvalContext& ctx) {
    if (!ctx.scene || mi.children.empty())
        return;
    for (u32 h : mi.children) {
        auto* child = ctx.scene->Actors().Find(h);
        if (!child || !child->ridesParentBone)
            continue;
        child->worldTransform = BoneRidingTransform(mi, state.boneWorldMatrices, *child);
        child->parentVisibility = mi.parentVisibility;
        child->mirrored = mi.mirrored;
    }
}
#endif

} // namespace

Matrix44f BoneRidingTransform(const Actor& parent, std::span<const Matrix44f> parentBones,
                              const Actor& child) {
    // `S * W == X * S * parentWorld`, so `W = S^-1 * X * parentScaled`. S is a
    // uniform scale times a basis swap, so its inverse is one matrix.
    Matrix44f x = child.attachParentOffset;
    const i32 bone = child.attachParentBone;
    if (bone >= 0 && bone < static_cast<i32>(parentBones.size()))
        x = x * parentBones[static_cast<usize>(bone)];

    const f32 inv = (child.worldScale > 0.0f) ? 1.0f / child.worldScale : 1.0f;
    Matrix44f invS = Matrix44f::scaling({inv, inv, inv});
    if (child.sourceSpace != kDefaultCoordSpace)
        invS = invS * CoordinateSystem::BasisChange(kDefaultCoordSpace, child.sourceSpace);
    return invS * (x * parent.ScaledWorldTransform());
}

void Actor::ApplyFrameState(const FrameState& state, i32 localTimeMs, const ActorEvalContext& ctx) {
    ApplyBoneMatrices(*this, state);
    render.ApplyGeosetStates(state);
    render.ApplyLayerStates(state);
    if (ctx.particles)
        ApplyParticleFrameStates(*this, state, *ctx.particles, ctx.camPos, ctx.view,
                                 ctx.frameDtMs);
    if (ctx.ribbons)
        ApplyRibbonFrameStates(*this, state, *ctx.ribbons, ctx.queryGround);
    if (ctx.particles)
        ApplyChildModelFrameStates(*this, state, *ctx.particles);
#if WDX_ENABLE_D3
    // Before ApplyD3ParticleFrames on purpose: an attachment firing this frame
    // registers its emitter here, and the call below is what puts it on its
    // hardpoint instead of leaving it a frame at the origin.
    if (ctx.fireEvents && !d3Attachments.Empty()) {
        i32 lo = 0, hi = 0x7FFFFFFF;
        SequenceWindowMs(*this, animation.ActiveSequenceIndex(), lo, hi);
        d3Attachments.Tick(*this, animation.ActiveSequenceIndex(), localTimeMs, lo, hi,
                           ctx.particles);
    }
    if (ctx.particles)
        ApplyD3ParticleFrames(*this, state, *ctx.particles, ctx);
    ApplyD3AttachedChildren(*this, state, ctx);
#endif

    for (i32 i = 0;
         i < (i32)state.collisionTransforms.size() && i < (i32)render.collisionShapes.size(); i++)
        render.collisionShapes[i].transform = state.collisionTransforms[i];

    // A physics body's type is per-frame, not per-file: StarCraft II keys it as
    // a channel and the physics stage resolves the inherit chains on top of
    // that, so a hero's kinematic proxies become a ragdoll's dynamic segments
    // mid-sequence. The shape list is per-template and cannot say that, which
    // is why the overlay's colour is refreshed here instead of at load. Static
    // is left alone — it is the one type that never switches.
    for (auto& cs : render.collisionShapes) {
        if (cs.bodyIndex < 0 || cs.bodyIndex >= (i32)state.physicsBodyDynamic.size())
            continue;
        const auto kind = static_cast<CollisionBodyKind>(cs.bodyKind);
        if (kind == CollisionBodyKind::None || kind == CollisionBodyKind::Static)
            continue;
        cs.bodyKind =
            static_cast<i32>(state.physicsBodyDynamic[cs.bodyIndex] ? CollisionBodyKind::Dynamic
                                                                    : CollisionBodyKind::Kinematic);
    }

    // The cloth overlay's per-frame half is a *gather*, not a transform: where
    // a particle is a palette node its live position is already sitting in the
    // matrix the shader will skin with, and where it is not the stage has left
    // it in `clothParticles`. Resolved here rather than in the debug pass so
    // that pass never has to read either.
    for (usize ci = 0; ci < render.cloths.size(); ci++) {
        auto& cloth = render.cloths[ci];
        const i32 ai = cloth.def.activeIndex;
        cloth.active =
            ai < 0 || ai >= (i32)state.clothActive.size() || state.clothActive[ai] != 0;
        // A solver that keeps its particles off the palette hands them over
        // directly instead (Diablo III); one whose particles *are* bones leaves
        // the channel empty and is gathered from the matrices below.
        if (ci < state.clothParticles.size() && !state.clothParticles[ci].empty()) {
            cloth.particles = state.clothParticles[ci];
        } else {
            cloth.particles.resize(cloth.def.particleNodes.size());
            for (usize i = 0; i < cloth.def.particleNodes.size(); i++) {
                const i32 n = cloth.def.particleNodes[i];
                cloth.particles[i] = (n >= 0 && n < (i32)state.boneWorldMatrices.size())
                                         ? Vector3f{state.boneWorldMatrices[n].data[3][0],
                                                    state.boneWorldMatrices[n].data[3][1],
                                                    state.boneWorldMatrices[n].data[3][2]}
                                         : Vector3f{0, 0, 0};
            }
        }
        cloth.colliders.resize(cloth.def.colliders.size());
        for (usize i = 0; i < cloth.def.colliders.size(); i++) {
            const auto& cd = cloth.def.colliders[i];
            cloth.colliders[i] = (cd.node >= 0 && cd.node < (i32)state.boneWorldMatrices.size())
                                     ? cd.local * state.boneWorldMatrices[cd.node]
                                     : cd.local;
        }
    }

    // Handed straight through: the spans point at solver storage the source
    // owns, and the upload that reads them runs later in this same frame.
    render.pendingDeforms = state.geosetDeforms;

    ApplyAttachmentStates(*this, state, ctx);
    ApplyCornFrameStates(*this, state, ctx);

    if (ctx.fireEvents && !events.Empty()) {
        const i32 activeSeq = animation.ActiveSequenceIndex();
        i32 seqStart = 0, seqEnd = 0x7FFFFFFF;
        SequenceWindowMs(*this, activeSeq, seqStart, seqEnd);
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
    req.view = ctx.view;
    FrameState fs = animation.Source()->Evaluate(req);
    ApplyFrameState(fs, localTime, ctx);
}

} // namespace whiteout::flakes::renderer::model
