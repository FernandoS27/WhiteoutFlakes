// The SC2 tick: emit, move, retire, and the spline's whole-ribbon clock.
//
// Everything here is a JOIN. `WriteHead` and `CatchUpTicks` are each measured
// against a golden on their own (ribbon_stages_sc2.cpp); what nothing measures
// is the ORDER they run in and what happens between two of them, which is what
// this file is. The frame loop follows RIBBON_SERVICE.md §5.1 and is gated
// structurally, not bit-for-bit.

#include "renderer/ribbon/ribbon_emitter.h"

#include "renderer/sc2/sc2_element.h"
#include "renderer/sc2/sc2_element_math.h"
#include "sim_util.h"

#include <algorithm>
#include <cmath>

namespace whiteout::flakes::renderer::ribbon {

namespace {

namespace vs = ::whiteout::flakes::renderer::sc2::vs;

// Terrain collision (W4) lives in sc2/sc2_element.h: a swept step against the
// ground query is element code, and the particle MOVE stage is its second
// caller (SC2_PARTICLE_DESIGN.md R6).
using ::whiteout::flakes::renderer::sc2::GroundCollide;
using ::whiteout::flakes::renderer::sc2::GroundHit;

} // namespace

// ---------------------------------------------------------------------------
// SC2 time-mode stages. The per-element launch state is the oracle-gated
// kernel; the frame loop is RIBBON_SERVICE.md §5.1 (design-driven): headU is
// the emission-time clock (seconds), a segment is committed each
// emitPeriod = P / (emissionScale·lodKeepFactor·divisions) with the last two
// factors 1 at viewer quality, and Simulate_Type0 retires whatever has aged
// past its deathU. Length/Legacy techniques (2/3/4) are W4.
// ---------------------------------------------------------------------------

f32 RibbonEmitter::SegmentsPerSecondSc2() const {
    // P = lifetime (time mode) / maxLength (length mode). rate = divisions / P
    // keeps `divisions` segments alive across one P.
    const f32 P = (desc_.sc2.cullMethod == CullMethod::Length) ? state_.sc2.maxLength
                                              : state_.sc2.lifetime;
    if (P <= 0.0f || desc_.sc2.divisions <= 0.0f)
        return 0.0f;
    return desc_.sc2.divisions / P;
}

/// Inherit-parent-velocity (`InheritParentVelocity`) and the stationary floor,
/// in the binary's order: fold the smoothed emitter velocity in, then test the
/// squared length of the POST-inherit vector and replace a degenerate one with
/// @p dir scaled by the same constant (UpdateHeadSegment steps 4→5).
///
/// The inherit gate does NOT test world/local — a LOCAL + inherit ribbon also
/// accumulates a nonzero smoothedDir and adds it to a raw local velocity, which
/// is the binary's own quirk (DRIFT-2), not an oversight to tidy away. Doing the
/// floor in local, pre-inherit space was DRIFT-1, which is why @p dir arrives
/// already in the element's space rather than being derived here.
void RibbonEmitter::ApplyInheritAndStationaryFloor(Vector3f& velocity, const Vector3f& dir,
                                                   SimTechnique tech) const {
    if (desc_.sc2.InheritsParentVelocity()) {
        const Vector3f sm = sc2_->smoothed.Value();
        const f32 k = state_.sc2.parentVelocityScale;
        velocity = {velocity.x + sm.x * k, velocity.y + sm.y * k, velocity.z + sm.z * k};
    }
    if (!AppliesStationaryFloor(tech))
        return;
    const f32 sq =
        (velocity.x * velocity.x + velocity.y * velocity.y) + velocity.z * velocity.z;
    if (sq < kSqStationaryFloor)
        velocity = {dir.x * kSqStationaryFloor, dir.y * kSqStationaryFloor,
                    dir.z * kSqStationaryFloor};
}

RibbonElement RibbonEmitter::MakeSegmentSc2(f32 birthU, f32 fracToCurr) const {
    sc2::HeadInputs in;
    in.simTechnique = desc_.sc2.simTechnique;
    in.ribbonType = desc_.sc2.ribbonType;
    in.cullMethod = desc_.sc2.cullMethod;
    in.swapYawPitch = desc_.sc2.SwapsYawPitch();
    in.headU = birthU;
    in.yawDeg = state_.sc2.yawDeg;
    in.pitchDeg = state_.sc2.pitchDeg;
    in.speed = state_.sc2.speed;
    in.lifetime = state_.sc2.lifetime;
    in.size3 = state_.sc2.size3;
    in.rotation3 = state_.sc2.rotation3;
    in.mass = desc_.sc2.mass;
    in.maxLengthBound = desc_.sc2.maxLengthBound;
    // Overlay waves (W6): the static types gate; the amplitude/frequency/phase
    // arrive per frame. The wave clock is the segment's own birthU (overlayTime
    // advances by dt exactly like the emission clock, RE §4.7).
    for (i32 i = 0; i < WaveChannel::kCount; ++i) {
        in.waveTypes[i] = desc_.sc2.waveTypes[i];
        in.waveAmp[i] = state_.sc2.waveAmp[i];
        in.waveFreq[i] = state_.sc2.waveFreq[i];
    }
    in.overlayPhase = state_.sc2.overlayPhase;
    in.overlayTime = birthU;
    const sc2::HeadElement h = sc2::WriteHead(in);

    RibbonElement e;
    // Two anchoring modes. Nearly every shipped hero ribbon is WORLD-space (the
    // corruptor tentacles, the zealot hair): the segment is born at the
    // emitter's world position AT birth time and its launch velocity/up are
    // rotated into world, so the strip traces the emitter's path through space
    // plus the drag arc — that path IS the tentacle spread. LOCAL-space births
    // at the emitter origin and BUILD transforms the whole strip by the current
    // world matrix (a rigid drag jet).
    //
    // Both spaces then run the SAME two steps in the same order; the only thing
    // the mode decides is whether the transform happens first. Writing the two
    // steps out per branch is how they came to be twenty duplicated lines.
    const bool world = desc_.sc2.IsWorldSpace();
    if (world) {
        // birthU sits between the previous and current pose; interpolate the
        // world origin the same fraction, like EmitWc3 interpolates its edges.
        e.birthPos = {prevPos_.x + (currPos_.x - prevPos_.x) * fracToCurr,
                      prevPos_.y + (currPos_.y - prevPos_.y) * fracToCurr,
                      prevPos_.z + (currPos_.z - prevPos_.z) * fracToCurr};
        e.velocity = whiteout::transform_normal(h.velocity, state_.transform);
        e.up = whiteout::transform_normal(h.up, state_.transform);
    } else {
        e.birthPos = {0, 0, 0};
        e.velocity = h.velocity;
        e.up = h.up;
    }
    // The emission direction in the element's OWN space, which is what the
    // floor substitutes for a degenerate velocity.
    const Vector3f dir =
        world ? whiteout::transform_normal(h.dir, state_.transform) : h.dir;
    ApplyInheritAndStationaryFloor(e.velocity, dir, in.simTechnique);

    e.pos = e.birthPos;
    e.size3 = h.size3;
    e.rotation3 = h.rotation3;
    e.invMass = h.invMass;
    e.birthU = h.birthU;
    e.deathU = h.deathU;
    // Colour stops, with the alpha overlay wave added and clamped (the head
    // kernel hands the wave back rather than owning the colours).
    for (i32 i = 0; i < ColorStop::kCount; ++i) {
        e.color3[i] = state_.sc2.color3[i];
        e.color3[i].w = std::clamp(e.color3[i].w + h.alphaWave, 0.0f, 1.0f);
    }
    return e;
}

void RibbonEmitter::TickSc2(f32 dt) {
    // A spline has no per-segment emission: Simulate_Spline rebuilds the whole
    // strip from four control points each frame, so it replaces the emit/move
    // stages rather than augmenting them (RE §3.4).
    if (desc_.sc2.hasSpline) {
        TickSplineSc2(dt);
        return;
    }
    TickCtx t;
    PrepSc2(t, dt);
    StepSc2(t.dt);
}

void RibbonEmitter::PrepSc2(TickCtx& t, f32 dt) {
    t.lifeSpan = state_.sc2.lifetime;
    t.firstTick = !updatedOnce_;
    updatedOnce_ = true;
    t.dt = (dt >= 0) ? dt : 0.0f;

    // Inherit-parent-velocity smoothing: push this tick's emitter motion into
    // the 8-tap ring (no-op unless inheriting + world-space).
    UpdateSmoothedVelocitySc2(t.dt);

    // Spawn pre-roll (CatchUpEmission): the first tick after seeding lays the
    // trail's age distribution so it does not pop in truncated. Without motion
    // history the pre-rolled segments sit at the spawn pose; their staggered
    // birthU/deathU is the part that matters. The tick count is the gated O5
    // kernel; each pre-roll tick is a full 33 ms emit + (legacy) move + retire.
    if (posSet_ && !sc2_->caughtUp && state_.sc2.active) {
        sc2_->caughtUp = true;
        const sc2::CatchUpResult cu = sc2::CatchUpTicks(
            desc_.sc2.cullMethod, state_.sc2.speed, state_.sc2.lifetime,
            state_.sc2.maxLength);
        if (!cu.earlyOut) {
            // Each pre-roll tick is a WHOLE tick — the same StepSc2 the frame
            // runs, so the two cannot drift apart when a stage is added.
            for (i32 k = 0; k < cu.ticks; ++k)
                StepSc2(kCatchUpTickSeconds);
        }
    }
}

void RibbonEmitter::AppendSc2(f32 dt) {
    const bool emitting = posSet_ && IsEmitterVisible(state_.visibility) &&
                          state_.sc2.active;
    if (emitting) {
        // The same carry the WC3 emit loop runs — one kernel, two dialects.
        const EmissionBurst burst =
            AdvanceEmissionCarry(sc2_->emitAccum, dt, SegmentsPerSecondSc2());
        for (i32 i = 0; i < burst.count; ++i) {
            const f32 frac = burst.Fraction(i + 1);
            edges_.push_back(MakeSegmentSc2(sc2_->headU + frac * dt, frac));
        }
    }
    // headU is the clock whether or not we emit, so a muted trail still ages
    // out (active gates NEW segments, never the live trail).
    sc2_->headU += dt;
}

void RibbonEmitter::RetireSc2() {
    // Segments are in birth order, oldest at the front. Retire from the front
    // whatever has aged past its deathU (Simulate_Type0's deathU < headU walk).
    const f32 headU = sc2_->headU;
    edges_.erase(edges_.begin(),
                 std::find_if(edges_.begin(), edges_.end(),
                              [headU](const RibbonElement& e) {
                                  return e.deathU >= headU;
                              }));
}

void RibbonEmitter::StepSc2(f32 dt) {
    // Append, integrate, retire. A Legacy ribbon integrates each segment's
    // stored velocity/position under gravity + drag before retiring; the
    // analytic techniques leave the motion to the closed form BUILD applies
    // (Simulate_Type0 is append + retire only).
    AppendSc2(dt);
    if (IntegratesOnCpu(desc_.sc2.simTechnique))
        IntegrateLegacySc2(dt);
    RetireSc2();
}

void RibbonEmitter::IntegrateLegacySc2(f32 dt) {
    if (dt <= 0.0f)
        return;
    const auto& s = desc_.sc2;
    const bool worldSpace = s.IsWorldSpace();
    // Acceleration = gravity3 (force fields are out of scope). World-space
    // positions live in renderer units, so gravity takes the model→renderer
    // factor there.
    const f32 sc = worldSpace ? state_.unitScale : 1.0f;
    const Vector3f a = {s.gravity3.x * sc, s.gravity3.y * sc, s.gravity3.z * sc};
    const f32 drag = (s.drag > 0.0f) ? s.drag : 0.01f;
    const f32 halfDt = 0.5f * dt;

    // Terrain collision (flags & 0x2, which also forces this technique): the
    // swept segment reflects off the ground query. It answers in scene space, so
    // world-space segments collide directly (identity map) while local ones map
    // there and back through the emitter transform — matching the binary, which
    // transforms a non-world segment to world before it collides.
    const bool collide = static_cast<bool>(state_.groundQuery) && s.CollidesTerrain();
    Matrix44f toScene = Matrix44f::identity(), fromScene = Matrix44f::identity();
    if (collide && !worldSpace) {
        toScene = state_.transform;
        fromScene = Matrix44f::inverse(state_.transform);
    }

    for (RibbonElement& e : edges_) {
        const Vector3f oldPos = e.pos;
        // Semi-implicit Euler (Simulate_Type4): pos += (0.5·a·dt + v)·dt using
        // the OLD velocity, then v += a·dt.
        e.pos = {e.pos.x + (halfDt * a.x + e.velocity.x) * dt,
                 e.pos.y + (halfDt * a.y + e.velocity.y) * dt,
                 e.pos.z + (halfDt * a.z + e.velocity.z) * dt};
        e.velocity = {e.velocity.x + a.x * dt, e.velocity.y + a.y * dt,
                      e.velocity.z + a.z * dt};

        // Collision sits between the integrate and the drag, as the binary does.
        if (collide) {
            const Vector3f oS = whiteout::transform_point(oldPos, toScene);
            const Vector3f nS = whiteout::transform_point(e.pos, toScene);
            const Vector3f vS = whiteout::transform_normal(e.velocity, toScene);
            const GroundHit gh =
                GroundCollide(oS, nS, vS, dt, s.friction, s.bounce, state_.groundQuery);
            if (gh.hit) {
                e.pos = whiteout::transform_point(gh.pos, fromScene);
                e.velocity = whiteout::transform_normal(gh.vel, fromScene);
            }
        }

        // Drag damping v *= max(1 − invMass·drag·dt, 0) (element+28 = 1/mass;
        // the floor dword_103C458E8 is 0.0, not the 1e-4 an earlier pass used).
        const f32 damping = (std::max)(1.0f - e.invMass * drag * dt, 0.0f);
        e.velocity = {e.velocity.x * damping, e.velocity.y * damping,
                      e.velocity.z * damping};
    }
}

void RibbonEmitter::UpdateSmoothedVelocitySc2(f32 dt) {
    // The smoothing ring is fed the world-matrix translation delta whenever
    // inherit (flags & 0x10) is set — EmitSegments' gate (0x102953761) does NOT
    // test world/local, so a LOCAL + inherit ribbon also accumulates a nonzero
    // smoothedDir (DRIFT-2). Only the head ELEMENT position is forced 0 for local
    // (A2); the marched world headPos/smoothedDir are not — the old "local
    // smoothedDir is always 0" was wrong.
    if (!desc_.sc2.InheritsParentVelocity())
        return;
    if (!posSet_ || dt <= 0.0f)
        return;
    sc2_->smoothed.Push({currPos_.x - prevPos_.x, currPos_.y - prevPos_.y,
                       currPos_.z - prevPos_.z},
                      dt);
}

// ---------------------------------------------------------------------------
// SC2 spline ribbons (technique 1 / CPU spline, RIBBON_SERVICE_PLAN.md W5).
// One cubic Bezier from the single SRIB record — a whole-ribbon shape rebuilt
// each frame, not a trail of aged segments. Verified against CRibbon_Simulate_
// Spline (4.8 0x10295E170): control-point construction, persistent age² sag,
// 32 samples at t = i/31, up = world +Z, V = 1 − t.
// ---------------------------------------------------------------------------

void RibbonEmitter::TickSplineSc2(f32 dt) {
    updatedOnce_ = true;
    sc2_->splineAge += (dt >= 0) ? dt : 0.0f;

    // Whole-ribbon death: past its lifetime the spline restarts (age and sag
    // reset), so a gravity-drooping spline loops rather than sagging forever
    // (RE §3.4: per-segment aging does not exist for splines).
    const f32 life = state_.sc2.lifetime;
    if (life > 0.0f && sc2_->splineAge >= life) {
        sc2_->splineAge = 0.0f;
        for (Vector3f& sg : sc2_->sag)
            sg = {0, 0, 0};
    }

    // Quadratic sag, per control point. The binary rotates gravity into
    // emitter-local, adds the sag there, then transforms the strip back to world
    // — a round trip that nets world-space gravity, so accel is gravity3
    // (renderer units) with no rotation. DEVIATION (O9): Simulate_Spline
    // ACCUMULATES `splineSagOffset += accel·dtAccumulator²` every frame (the
    // gate's `sag-twice` vector doubles it) with dtAccumulator = the total age,
    // and the driver never zeros it, so the retail droop is accel·Σage_i². We
    // recompute accel·age² fresh instead: identical on the first frame and a
    // bounded quadratic droop rather than the retail runaway. gravity3 == 0 —
    // the common case — leaves the spline rigid, where the two agree exactly.
    const Vector3f g = desc_.sc2.gravity3;
    const f32 age2 = sc2_->splineAge * sc2_->splineAge * state_.unitScale;
    const Vector3f d = {g.x * age2, g.y * age2, g.z * age2};
    for (Vector3f& sg : sc2_->sag)
        sg = d;
}
} // namespace whiteout::flakes::renderer::ribbon
