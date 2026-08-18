#include "renderer/ribbon/ribbon_emitter.h"

#include "constants.h"
#include "sim_util.h"

#include <algorithm>
#include <cmath>

namespace whiteout::flakes::renderer::ribbon {

RibbonDesc DescFromWc3Config(const RibbonEmitterConfig& cfg) {
    RibbonDesc d;
    d.edgesPerSecond = cfg.emission;
    d.edgeLifespan = cfg.life;
    d.gravity = cfg.gravity;
    d.rows = cfg.rows;
    d.cols = cfg.cols;
    d.priorityPlane = cfg.priorityPlane;
    // An MDX ribbon is one pass, so the scalar fields ARE its single layer.
    // A config that filled `layers` (the `.m2` route) keeps them verbatim.
    if (cfg.layers.empty()) {
        RibbonLayer l;
        l.textureId = cfg.textureId;
        l.filterMode = cfg.filterMode;
        l.unshaded = cfg.unshaded;
        l.twoSided = cfg.twoSided;
        d.layers = {l};
    } else {
        d.layers = cfg.layers;
    }
    return d;
}

f32 RibbonEmitter::SimLifespan() const {
    if (behavior_.lifespanFloorAppliesToSim)
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
    if (behavior_.requirePositiveDtToEmit && dt <= 0)
        return false;
    if (behavior_.requirePositiveRateToEmit && desc_.edgesPerSecond <= 0)
        return false;
    return true;
}

void RibbonEmitter::Update(f32 dt) {
    const f32 lifeSpan = SimLifespan();
    const bool firstTick = !updatedOnce_;

    if (behavior_.firstFrameEmitsOneEdge && firstTick && desc_.edgesPerSecond > 0)
        dt = 1.0f / desc_.edgesPerSecond + 1e-4f;
    updatedOnce_ = true;

    if (behavior_.clampDtToLifespan) {
        dt = (dt >= 0) ? (std::min)(dt, lifeSpan) : 0.0f;
    } else {
        dt = ClampDeltaTime(dt);
        // Past a full lifespan nothing in the trail could survive, so the MDX
        // path restarts it at the current pose rather than retiring edge by
        // edge. WoW clamps dt instead, which is why this is behaviour-gated.
        if (dt >= lifeSpan) {
            edges_.clear();
            headPending_ = false;
            prevPos_ = currPos_;
            prevDir_ = currDir_;
            prevVertical_ = currVertical_;
            accumEmission_ = 0;
            dt = 0;
        }
    }

    // Retire from the tail. A provisional head sits at the ring's write
    // position, outside the live range, so it is never a retirement candidate.
    const usize committed = edges_.size() - ((headPending_ && !edges_.empty()) ? 1u : 0u);
    const auto committedEnd = edges_.begin() + static_cast<std::ptrdiff_t>(committed);
    edges_.erase(edges_.begin(),
                 std::find_if(edges_.begin(), committedEnd, [lifeSpan, dt](const RibbonEdge& e) {
                     return e.age < (lifeSpan - dt);
                 }));

    bool emittedHead = false;
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
            !(behavior_.skipEmitWhenStationary && dist < 0.001f && !firstTick);

        // The provisional head from last frame is overwritten by this frame's
        // emission rather than kept.
        if (headPending_) {
            edges_.pop_back();
            headPending_ = false;
        }

        const f32 endTime = accumEmission_ + dt * desc_.edgesPerSecond;
        f32 newEdgeTime = 1.0f;

        if (endTime >= 1.0f && interpValid) {
            const i32 numNew = (i32)floorf(endTime - newEdgeTime) + 1;
            const f32 ooDenom = (endTime - accumEmission_ > kVectorEpsilon)
                                    ? 1.0f / (endTime - accumEmission_)
                                    : 1.0f;

            const Vector3f prevDirS = {prevDir_.x * dist, prevDir_.y * dist, prevDir_.z * dist};
            const Vector3f currDirS = {currDir_.x * dist, currDir_.y * dist, currDir_.z * dist};

            const Vector3f above0 = {prevPos_.x + prevVertical_.x * above,
                                     prevPos_.y + prevVertical_.y * above,
                                     prevPos_.z + prevVertical_.z * above};
            const Vector3f above1 = {currPos_.x + currVertical_.x * above,
                                     currPos_.y + currVertical_.y * above,
                                     currPos_.z + currVertical_.z * above};
            const Vector3f below0 = {prevPos_.x - prevVertical_.x * below,
                                     prevPos_.y - prevVertical_.y * below,
                                     prevPos_.z - prevVertical_.z * below};
            const Vector3f below1 = {currPos_.x - currVertical_.x * below,
                                     currPos_.y - currVertical_.y * below,
                                     currPos_.z - currVertical_.z * below};

            for (i32 i = 0; i < numNew; ++i) {
                f32 t = (newEdgeTime - accumEmission_) * ooDenom;
                t = std::clamp(t, 0.0f, 1.0f);
                const f32 omt = 1.0f - t;

                RibbonEdge e;
                e.bot = {(below0.x + prevDirS.x * t) * omt + (below1.x - currDirS.x * omt) * t,
                         (below0.y + prevDirS.y * t) * omt + (below1.y - currDirS.y * omt) * t,
                         (below0.z + prevDirS.z * t) * omt + (below1.z - currDirS.z * omt) * t};
                e.top = {(above0.x + prevDirS.x * t) * omt + (above1.x - currDirS.x * omt) * t,
                         (above0.y + prevDirS.y * t) * omt + (above1.y - currDirS.y * omt) * t,
                         (above0.z + prevDirS.z * t) * omt + (above1.z - currDirS.z * omt) * t};
                e.age = -dt * t;
                edges_.push_back(e);
                newEdgeTime += 1.0f;
            }
        }

        accumEmission_ = endTime - floorf(endTime);

        RibbonEdge head;
        head.top = {currPos_.x + currVertical_.x * above,
                    currPos_.y + currVertical_.y * above,
                    currPos_.z + currVertical_.z * above};
        head.bot = {currPos_.x - currVertical_.x * below,
                    currPos_.y - currVertical_.y * below,
                    currPos_.z - currVertical_.z * below};
        head.age = 0;
        edges_.push_back(head);
        emittedHead = true;
        headPending_ = behavior_.headEdgeIsProvisional;
    }

    // The head placed this frame is not aged; neither is a provisional one
    // carried over from an earlier frame, since it lives outside the ring's
    // live range.
    const usize skipTail = (emittedHead || headPending_) ? 1u : 0u;
    const usize updateEnd = edges_.size() - (std::min)(skipTail, edges_.size());
    for (usize i = 0; i < updateEnd; ++i) {
        auto& e = edges_[i];
        // g*dt^2 + 2*g*age*dt == g*((age+dt)^2 - age^2), so the closed form is
        // z0 + g*t^2 — not the textbook 0.5*g*t^2. Halving it drifts by 2x.
        // Model units per second squared, like the half-widths above.
        const f32 g = desc_.gravity * state_.unitScale;
        const f32 fall = behavior_.gravitySign * (g * dt * dt + 2.0f * g * e.age * dt);
        e.top.z += fall;
        e.bot.z += fall;
        e.age += dt;
    }
}

i32 RibbonEmitter::VertexCount() const {
    const i32 n = (i32)edges_.size();
    return (n > 1) ? (n - 1) * 6 : 0;
}

i32 RibbonEmitter::BuildStrip(std::vector<Vertex>& out) const {
    if (state_.visibility <= 0.0f || edges_.size() < 2)
        return 0;

    // Sprite-sheet cell, in the client's own axis assignment: Initialize sets
    // tmpDU = texBox.width / ROWS and tmpDV = texBox.height / COLS (texBox is
    // {0,0,1,1} for every `.m2`), and SetTexSlot @0x100e7d460 then indexes U by
    // slot/cols and V by slot%cols. That is the transpose of the obvious
    // reading, and it is unobservable in shipped data — every one of the
    // corpus's 5293 ribbons is 1x1 — so it is transcribed, not inferred.
    // SetTexSlot asserts `slot < m_rows * m_cols`; the slot arrives from an
    // animation track, so clamp instead of trusting it into the divide.
    const i32 rows = (std::max)(desc_.rows, 1);
    const i32 cols = (std::max)(desc_.cols, 1);
    const i32 slot = std::clamp(state_.slot, 0, rows * cols - 1);

    const f32 cellU = 1.0f / static_cast<f32>(rows);
    const f32 cellV = 1.0f / static_cast<f32>(cols);
    const i32 slotRow = slot / cols;
    const i32 slotCol = slot % cols;
    const f32 texL = cellU * slotRow;
    const f32 texT = cellV * slotCol;
    const f32 texB = texT + cellV;
    const f32 texDU = cellU;

    const f32 ooLife = 1.0f / SimLifespan();

    const Vector4f vertColor = {state_.color.x, state_.color.y, state_.color.z, state_.alpha};
    const Vector3f normal = {1, 0, 0};

    // The emitter's texture transform, applied last and per vertex. The client
    // hands it to the stage as a matrix and transforms per fragment, but it is
    // affine in uv, so transforming the corners and interpolating is the same
    // result — and it keeps the ribbon draw a plain textured strip.
    const f32* const r0 = state_.texAnimRow0;
    const f32* const r1 = state_.texAnimRow1;
    const auto uv = [r0, r1](f32 u, f32 v) -> Vector2f {
        return {r0[0] * u + r0[1] * v + r0[3], r1[0] * u + r1[1] * v + r1[3]};
    };

    const i32 before = (i32)out.size();
    const i32 numEdges = (i32)edges_.size();
    for (i32 i = 0; i < numEdges - 1; i++) {
        const auto& e0 = edges_[i];
        const auto& e1 = edges_[i + 1];

        const f32 u0 = texDU * e0.age * ooLife + texL;
        const f32 u1 = texDU * e1.age * ooLife + texL;

        out.push_back({e0.top, normal, vertColor, uv(u0, texT)});
        out.push_back({e0.bot, normal, vertColor, uv(u0, texB)});
        out.push_back({e1.top, normal, vertColor, uv(u1, texT)});

        out.push_back({e0.bot, normal, vertColor, uv(u0, texB)});
        out.push_back({e1.bot, normal, vertColor, uv(u1, texB)});
        out.push_back({e1.top, normal, vertColor, uv(u1, texT)});
    }
    return (i32)out.size() - before;
}

} // namespace whiteout::flakes::renderer::ribbon
