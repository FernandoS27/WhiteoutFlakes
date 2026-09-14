#include "renderer/ribbon/ribbon_emitter.h"

#include "constants.h"
#include "sim_util.h"

#include <algorithm>
#include <cmath>

namespace whiteout::flakes::renderer::ribbon {

f32 RibbonEmitter::SimLifespan() const {
    if (desc_.behavior.lifespanFloorAppliesToSim)
        return (std::max)(desc_.edgeLifespan, kRibbonMinLifespan);
    return desc_.edgeLifespan;
}

void RibbonEmitter::SetState(const RibbonState& st) {
    state_ = st;

    const Vector3f newPos = whiteout::transform_point(Vector3f{0, 0, 0}, st.transform);
    const Vector3f newDir = whiteout::transform_normal(Vector3f{0, 0, 1}, st.transform).normalized();
    const Vector3f newVert =
        whiteout::transform_normal(Vector3f{0, 1, 0}, st.transform).normalized();

    if (posSet_) {
        prevPos_ = currPos_;
        prevDir_ = currDir_;
        prevVertical_ = currVertical_;
    } else {
        prevPos_ = newPos;
        prevDir_ = newDir;
        prevVertical_ = newVert;
        accumEmission_ = 0;
        posSet_ = true;
    }
    currPos_ = newPos;
    currDir_ = newDir;
    currVertical_ = newVert;
}

bool RibbonEmitter::ShouldEmit(f32 dt) const {
    if (!posSet_ || !IsEmitterVisible(state_.visibility))
        return false;
    if (desc_.behavior.requirePositiveDtToEmit && dt <= 0)
        return false;
    if (desc_.behavior.requirePositiveRateToEmit && desc_.edgesPerSecond <= 0)
        return false;
    return true;
}

void RibbonEmitter::SetDesc(const RibbonDesc& d) {
    desc_ = d;
    // The runtime block and the family selector are installed together, so
    // nothing downstream can find an SC2 desc without its clocks — or a WC3
    // emitter carrying them (R1/R8).
    if (desc_.family == RibbonDesc::Family::Sc2) {
        if (!sc2_)
            sc2_ = std::make_unique<Sc2Runtime>();
    } else {
        sc2_.reset();
    }
}

void RibbonEmitter::Update(f32 dt) {
    // One tick = the family's stage variants, and this is the only place the
    // family is read on the simulation side. Each arm is a whole tick rather
    // than a stage, so the selector is answered once instead of once per stage.
    switch (desc_.family) {
    case RibbonDesc::Family::Wc3:
        TickWc3(dt);
        return;
    case RibbonDesc::Family::Sc2:
        TickSc2(dt);
        return;
    }
}

void RibbonEmitter::TickWc3(f32 dt) {
    // The client's own order: retire, then emit, then move.
    TickCtx t;
    PrepWc3(t, dt);
    RetireWc3(t);
    EmitWc3(t);
    MoveWc3(t);
}

void RibbonEmitter::PrepWc3(TickCtx& t, f32 dt) {
    t.lifeSpan = SimLifespan();
    t.firstTick = !updatedOnce_;

    if (desc_.behavior.firstFrameEmitsOneEdge && t.firstTick && desc_.edgesPerSecond > 0)
        dt = 1.0f / desc_.edgesPerSecond + 1e-4f;
    updatedOnce_ = true;

    if (desc_.behavior.clampDtToLifespan) {
        dt = (dt >= 0) ? (std::min)(dt, t.lifeSpan) : 0.0f;
    } else {
        dt = ClampDeltaTime(dt);
        // Past a full lifespan nothing in the trail could survive, so the MDX
        // path restarts it at the current pose rather than retiring edge by
        // edge. WoW clamps dt instead, which is why this is behaviour-gated.
        if (dt >= t.lifeSpan) {
            edges_.clear();
            headPending_ = false;
            prevPos_ = currPos_;
            prevDir_ = currDir_;
            prevVertical_ = currVertical_;
            accumEmission_ = 0;
            dt = 0;
        }
    }
    t.dt = dt;
}

void RibbonEmitter::RetireWc3(const TickCtx& t) {
    const f32 lifeSpan = t.lifeSpan;
    const f32 dt = t.dt;
    // Retire from the tail. A provisional head sits at the ring's write
    // position, outside the live range, so it is never a retirement candidate.
    const usize committed = edges_.size() - ((headPending_ && !edges_.empty()) ? 1u : 0u);
    const auto committedEnd = edges_.begin() + static_cast<std::ptrdiff_t>(committed);
    edges_.erase(edges_.begin(),
                 std::find_if(edges_.begin(), committedEnd,
                              [lifeSpan, dt](const RibbonElement& e) {
                                  return e.age < (lifeSpan - dt);
                              }));
}

void RibbonEmitter::EmitWc3(TickCtx& t) {
    const f32 dt = t.dt;
    const bool firstTick = t.firstTick;
    if (ShouldEmit(dt)) {
        // The record's half-widths are model units; the edges are renderer ones.
        const f32 above = state_.above * state_.unitScale;
        const f32 below = state_.below * state_.unitScale;

        const f32 dx = currPos_.x - prevPos_.x;
        const f32 dy = currPos_.y - prevPos_.y;
        const f32 dz = currPos_.z - prevPos_.z;
        const f32 dist = sqrtf(dx * dx + dy * dy + dz * dz);

        // InitInterpDeltas' early-out. It gates the emission loop ONLY: a
        // stationary ribbon still advances its carry and still re-places its
        // head at the current pose, exactly as Update does past LABEL_33. The
        // first tick is exempt, which is what lets a ribbon that spawns
        // stationary show its head at all.
        const bool interpValid =
            !(desc_.behavior.skipEmitWhenStationary && dist < kWc3StationaryDistance && !firstTick);

        // The provisional head from last frame is overwritten by this frame's
        // emission rather than kept.
        if (headPending_) {
            edges_.pop_back();
            headPending_ = false;
        }

        // The carry advances whether or not the loop runs: a stationary WoW
        // ribbon still banks its fraction and still re-places its head.
        const EmissionBurst burst =
            AdvanceEmissionCarry(accumEmission_, dt, desc_.edgesPerSecond);

        if (burst.count > 0 && interpValid) {
            const Vector3f prevDirS = {prevDir_.x * dist, prevDir_.y * dist, prevDir_.z * dist};
            const Vector3f currDirS = {currDir_.x * dist, currDir_.y * dist, currDir_.z * dist};

            // The four extremes the blend runs between: the previous and
            // current pose, each offset along its own vertical by the record's
            // half-widths.
            const auto offsetBy = [](const Vector3f& p, const Vector3f& up, f32 d) {
                return Vector3f{p.x + up.x * d, p.y + up.y * d, p.z + up.z * d};
            };
            const Vector3f above0 = offsetBy(prevPos_, prevVertical_, above);
            const Vector3f above1 = offsetBy(currPos_, currVertical_, above);
            const Vector3f below0 = offsetBy(prevPos_, prevVertical_, -below);
            const Vector3f below1 = offsetBy(currPos_, currVertical_, -below);

            // InterpEdge, operand for operand — the tangent scaling by
            // |curr-prev| and the order of the two products are the golden
            // contract, so this expression is transcribed, never simplified.
            const auto interpEdge = [&](const Vector3f& p0, const Vector3f& p1, f32 t,
                                        f32 omt) {
                return Vector3f{(p0.x + prevDirS.x * t) * omt + (p1.x - currDirS.x * omt) * t,
                                (p0.y + prevDirS.y * t) * omt + (p1.y - currDirS.y * omt) * t,
                                (p0.z + prevDirS.z * t) * omt + (p1.z - currDirS.z * omt) * t};
            };

            for (i32 i = 0; i < burst.count; ++i) {
                const f32 t = burst.Fraction(i + 1);
                const f32 omt = 1.0f - t;

                RibbonElement e;
                e.bot = interpEdge(below0, below1, t, omt);
                e.top = interpEdge(above0, above1, t, omt);
                e.age = -dt * t;
                edges_.push_back(e);
            }
        }

        RibbonElement head;
        head.top = {currPos_.x + currVertical_.x * above,
                    currPos_.y + currVertical_.y * above,
                    currPos_.z + currVertical_.z * above};
        head.bot = {currPos_.x - currVertical_.x * below,
                    currPos_.y - currVertical_.y * below,
                    currPos_.z - currVertical_.z * below};
        head.age = 0;
        edges_.push_back(head);
        t.emittedHead = true;
        headPending_ = desc_.behavior.headEdgeIsProvisional;
    }
}

void RibbonEmitter::MoveWc3(const TickCtx& t) {
    const f32 dt = t.dt;
    // The head placed this frame is not aged; neither is a provisional one
    // carried over from an earlier frame, since it lives outside the ring's
    // live range.
    const usize skipTail = (t.emittedHead || headPending_) ? 1u : 0u;
    const usize updateEnd = edges_.size() - (std::min)(skipTail, edges_.size());
    for (usize i = 0; i < updateEnd; ++i) {
        auto& e = edges_[i];
        // g*dt^2 + 2*g*age*dt == g*((age+dt)^2 - age^2), so the closed form is
        // z0 + g*t^2 — not the textbook 0.5*g*t^2. Halving it drifts by 2x.
        // Model units per second squared, like the half-widths above.
        const f32 g = desc_.gravity * state_.unitScale;
        const f32 fall = desc_.behavior.gravitySign * (g * dt * dt + 2.0f * g * e.age * dt);
        e.top.z += fall;
        e.bot.z += fall;
        e.age += dt;
    }
}
} // namespace whiteout::flakes::renderer::ribbon
