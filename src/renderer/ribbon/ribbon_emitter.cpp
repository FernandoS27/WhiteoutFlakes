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
    d.textureId = cfg.textureId;
    d.filterMode = cfg.filterMode;
    d.unshaded = cfg.unshaded;
    d.twoSided = cfg.twoSided;
    d.priorityPlane = cfg.priorityPlane;
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

            const Vector3f above0 = {prevPos_.x + prevVertical_.x * state_.above,
                                     prevPos_.y + prevVertical_.y * state_.above,
                                     prevPos_.z + prevVertical_.z * state_.above};
            const Vector3f above1 = {currPos_.x + currVertical_.x * state_.above,
                                     currPos_.y + currVertical_.y * state_.above,
                                     currPos_.z + currVertical_.z * state_.above};
            const Vector3f below0 = {prevPos_.x - prevVertical_.x * state_.below,
                                     prevPos_.y - prevVertical_.y * state_.below,
                                     prevPos_.z - prevVertical_.z * state_.below};
            const Vector3f below1 = {currPos_.x - currVertical_.x * state_.below,
                                     currPos_.y - currVertical_.y * state_.below,
                                     currPos_.z - currVertical_.z * state_.below};

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
        head.top = {currPos_.x + currVertical_.x * state_.above,
                    currPos_.y + currVertical_.y * state_.above,
                    currPos_.z + currVertical_.z * state_.above};
        head.bot = {currPos_.x - currVertical_.x * state_.below,
                    currPos_.y - currVertical_.y * state_.below,
                    currPos_.z - currVertical_.z * state_.below};
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
        const f32 fall =
            behavior_.gravitySign * (desc_.gravity * dt * dt + 2.0f * desc_.gravity * e.age * dt);
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

    const f32 cellW = (desc_.cols > 0) ? 1.0f / desc_.cols : 1.0f;
    const f32 cellH = (desc_.rows > 0) ? 1.0f / desc_.rows : 1.0f;
    const i32 slotRow = (desc_.cols > 0) ? state_.slot / desc_.cols : 0;
    const i32 slotCol = (desc_.cols > 0) ? state_.slot % desc_.cols : 0;
    const f32 texL = cellW * slotCol;
    const f32 texT = cellH * slotRow;
    const f32 texB = texT + cellH;
    const f32 texDU = (texL + cellW) - texL;

    const f32 ooLife = 1.0f / SimLifespan();

    const Vector4f vertColor = {state_.color.x, state_.color.y, state_.color.z, state_.alpha};
    const Vector3f normal = {1, 0, 0};

    const i32 before = (i32)out.size();
    const i32 numEdges = (i32)edges_.size();
    for (i32 i = 0; i < numEdges - 1; i++) {
        const auto& e0 = edges_[i];
        const auto& e1 = edges_[i + 1];

        const f32 u0 = texDU * e0.age * ooLife + texL;
        const f32 u1 = texDU * e1.age * ooLife + texL;

        out.push_back({e0.top, normal, vertColor, {u0, texT}});
        out.push_back({e0.bot, normal, vertColor, {u0, texB}});
        out.push_back({e1.top, normal, vertColor, {u1, texT}});

        out.push_back({e0.bot, normal, vertColor, {u0, texB}});
        out.push_back({e1.bot, normal, vertColor, {u1, texB}});
        out.push_back({e1.top, normal, vertColor, {u1, texT}});
    }
    return (i32)out.size() - before;
}

} // namespace whiteout::flakes::renderer::ribbon
