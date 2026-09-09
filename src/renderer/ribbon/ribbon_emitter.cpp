#include "renderer/ribbon/ribbon_emitter.h"

#include "constants.h"
#include "renderer/ribbon/ribbon_vs_math.h"
#include "sim_util.h"

#include <algorithm>
#include <cmath>
#include <functional>

namespace whiteout::flakes::renderer::ribbon {

namespace {

namespace vs = whiteout::flakes::renderer::ribbon::vs;

/// One centreline sample ready to expand: world position, the frame inputs, and
/// the interpolated per-vertex values. Both the time/length and spline BUILD
/// paths fill a list of these and hand it to EmitSc2Strip, so the cross-section
/// math (Ribbon.fx §4.1/§4.7) lives once regardless of how the centreline was
/// produced.
struct StripNode {
    Vector3f pos, tangent, up;
    f32 size, twist, v;
    Vector4f color;
};

/// Expand a node centreline into the ribbon's cross-section: a flat two-corner
/// strip for billboard/planar, a closed ring bridged rung to rung for
/// cylinder/star. Appends triangles to `out`, returns the vertex count added.
i32 EmitSc2Strip(const std::vector<StripNode>& nodes, int xsec, int ringEdges,
                 bool tube, bool smoothPath, const Vector3f& camDir,
                 f32 innerRadius, std::vector<Vertex>& out) {
    const i32 before = (i32)out.size();

    if (!tube) {
        // Billboard / planar: two edge vertices per node, bridged into quads.
        // U runs across the width (0/1); V is the node's V along the length.
        struct Edge {
            Vector3f top, bot, normal;
            Vector4f color;
            f32 v;
        };
        std::vector<Edge> ev;
        ev.reserve(nodes.size());
        for (const StripNode& n : nodes) {
            const vs::Frame f = vs::BuildFrame(xsec, n.tangent, n.up, camDir,
                                               1.0f, 1.0f, n.twist, smoothPath);
            const Vector3f half = vs::Scale(f.offset, n.size);
            ev.push_back({vs::Add(n.pos, half), vs::Sub(n.pos, half), f.normal,
                          n.color, n.v});
        }
        for (usize i = 0; i + 1 < ev.size(); ++i) {
            const Edge& a = ev[i];
            const Edge& b = ev[i + 1];
            out.push_back({a.top, a.normal, a.color, {0.0f, a.v}});
            out.push_back({a.bot, a.normal, a.color, {1.0f, a.v}});
            out.push_back({b.top, b.normal, b.color, {0.0f, b.v}});
            out.push_back({a.bot, a.normal, a.color, {1.0f, a.v}});
            out.push_back({b.bot, b.normal, b.color, {1.0f, b.v}});
            out.push_back({b.top, b.normal, b.color, {0.0f, b.v}});
        }
        return (i32)out.size() - before;
    }

    // Cylinder / star: a ring of `ringEdges` vertices per node — the section
    // perpendicular to the trail — with consecutive rings bridged into a closed
    // tube (Ribbon.fx:521). The ring plane is BuildFrame's post-twist
    // normal/binormal; the outward offset is the vertex normal, so the tube
    // shades as a tube instead of a flat strip that vanishes edge-on.
    const f32 kTwoPi = 6.2831853071795864769f;
    auto ringAxes = [&](const StripNode& n, Vector3f& N, Vector3f& B) {
        const Vector3f t = n.tangent;
        Vector3f binormal, normal;
        if (smoothPath) {
            binormal = n.up;
            normal = vs::SafeNormalize(vs::Cross3(t, binormal), Vector3f{0, 0, 1});
        } else {
            binormal = vs::SafeNormalize(vs::Cross3(t, n.up), Vector3f{0, 1, 0});
            normal = vs::SafeNormalize(vs::Cross3(t, binormal), Vector3f{0, 0, 1});
        }
        const vs::Mat3 rot = vs::MakeRotation(n.twist, t);
        B = vs::MulVecMat3(binormal, rot);
        N = vs::MulVecMat3(normal, rot);
    };
    // One ring vertex: outward direction (normalized for a cylinder; the star
    // pinches odd spokes to innerRadius, Ribbon.fx:527) and its world position.
    auto ringVert = [&](const StripNode& n, const Vector3f& N, const Vector3f& B,
                        int k, Vector3f& pos, Vector3f& nrm) {
        const f32 th = kTwoPi * (f32)k / (f32)ringEdges;
        const f32 cs = std::cos(th), sn = std::sin(th);
        Vector3f o;
        if (xsec == 2)
            o = vs::SafeNormalize(vs::Add(vs::Scale(N, cs), vs::Scale(B, sn)),
                                  Vector3f{0, 0, 1});
        else {
            // Star (BuildCrossSection type 3): EVEN spokes pinch to innerRadius,
            // odd spokes stay at the outer radius — index 0 is inner. The ring
            // holds 2·edges points (set at the call site), one inner + one outer
            // per authored edge.
            const f32 mag = ((k & 1) == 0) ? innerRadius : 1.0f;
            o = vs::Add(vs::Scale(N, cs * mag), vs::Scale(B, sn * mag));
        }
        nrm = vs::SafeNormalize(o, Vector3f{0, 0, 1});
        pos = vs::Add(n.pos, vs::Scale(o, n.size));
    };
    for (usize i = 0; i + 1 < nodes.size(); ++i) {
        Vector3f Na, Ba, Nb, Bb;
        ringAxes(nodes[i], Na, Ba);
        ringAxes(nodes[i + 1], Nb, Bb);
        const Vector4f ca = nodes[i].color, cb = nodes[i + 1].color;
        const f32 va = nodes[i].v, vb = nodes[i + 1].v;
        for (int k = 0; k < ringEdges; ++k) {
            const int k1 = (k + 1) % ringEdges;
            const f32 ua = (f32)k / (f32)ringEdges;
            const f32 ub = (f32)(k + 1) / (f32)ringEdges;
            Vector3f a0, a1, b0, b1, na0, na1, nb0, nb1;
            ringVert(nodes[i], Na, Ba, k, a0, na0);
            ringVert(nodes[i], Na, Ba, k1, a1, na1);
            ringVert(nodes[i + 1], Nb, Bb, k, b0, nb0);
            ringVert(nodes[i + 1], Nb, Bb, k1, b1, nb1);
            out.push_back({a0, na0, ca, {ua, va}});
            out.push_back({a1, na1, ca, {ub, va}});
            out.push_back({b0, nb0, cb, {ua, vb}});
            out.push_back({a1, na1, ca, {ub, va}});
            out.push_back({b1, nb1, cb, {ub, vb}});
            out.push_back({b0, nb0, cb, {ua, vb}});
        }
    }
    return (i32)out.size() - before;
}

/// Math_MatrixFromYawPitchRoll (0x100d56240) with roll = 0, as a row-major 3×3
/// so `v·M` (MulVecMat3) matches the engine: row 2 = (sin yaw, −sin pitch·cos
/// yaw, cos pitch·cos yaw) is the emission direction. `swap` is RIB_ flags &
/// 0x8000 (yaw/pitch order swap), matching Sc2WriteHead's a2/a3 assignment.
vs::Mat3 YawPitchMat(f32 yawDeg, f32 pitchDeg, bool swap) {
    constexpr f32 kDegToRad = 0.017453292f;
    const f32 a2 = (swap ? yawDeg : pitchDeg) * kDegToRad;
    const f32 a3 = (swap ? pitchDeg : yawDeg) * kDegToRad;
    const f32 s2 = std::sin(a2), c2 = std::cos(a2);
    const f32 s3 = std::sin(a3), c3 = std::cos(a3);
    vs::Mat3 m{};
    m.m[0][0] = c3;   m.m[0][1] = s3 * s2;   m.m[0][2] = -s3 * c2;
    m.m[1][0] = 0.0f; m.m[1][1] = c2;        m.m[1][2] = s2;
    m.m[2][0] = s3;   m.m[2][1] = -s2 * c3;  m.m[2][2] = c3 * c2;
    return m;
}

// --- Noise displacement (W6) -------------------------------------------------
// Retail samples a global coherent-noise field; ours is a MixSeed-seeded value
// noise — a stated deviation (RIBBON_SERVICE.md §9). The gate that matters is
// the sample coordinates and the edge mute (RE §4.3), which this reproduces;
// the field's exact values are compared curve-shape, not bit-exact.
f32 NoiseHash3(i32 xi, i32 yi, i32 zi) {
    u32 h = static_cast<u32>(xi * 73856093) ^ static_cast<u32>(yi * 19349663) ^
            static_cast<u32>(zi * 83492791);
    h ^= h >> 13;
    h *= 0x5bd1e995u;
    h ^= h >> 15;
    return static_cast<f32>(h & 0xFFFFFFu) * (2.0f / 16777215.0f) - 1.0f; // [-1,1]
}

/// Trilinearly-interpolated value noise in [-1, 1], smoothstep-weighted.
f32 NoiseField(f32 x, f32 y, f32 z) {
    const f32 fx = std::floor(x), fy = std::floor(y), fz = std::floor(z);
    const i32 x0 = static_cast<i32>(fx), y0 = static_cast<i32>(fy), z0 = static_cast<i32>(fz);
    auto sm = [](f32 t) { return t * t * (3.0f - 2.0f * t); };
    const f32 tx = sm(x - fx), ty = sm(y - fy), tz = sm(z - fz);
    auto lerp = [](f32 a, f32 b, f32 t) { return a + (b - a) * t; };
    const f32 c00 = lerp(NoiseHash3(x0, y0, z0), NoiseHash3(x0 + 1, y0, z0), tx);
    const f32 c10 = lerp(NoiseHash3(x0, y0 + 1, z0), NoiseHash3(x0 + 1, y0 + 1, z0), tx);
    const f32 c01 = lerp(NoiseHash3(x0, y0, z0 + 1), NoiseHash3(x0 + 1, y0, z0 + 1), tx);
    const f32 c11 = lerp(NoiseHash3(x0, y0 + 1, z0 + 1), NoiseHash3(x0 + 1, y0 + 1, z0 + 1), tx);
    return lerp(lerp(c00, c10, ty), lerp(c01, c11, ty), tz);
}

/// The 3-channel noise displacement at trail parameter `t` (RE §4.3): the field
/// sampled at (t·frequency, coherence·headU, {0, 0.33, 0.66}) × amplitude, muted
/// by t/edge near the head (`bothEnds` adds the (1−t)/edge far-end mute splines
/// use). Returns a positional offset in the ribbon's own space.
Vector3f NoiseDisplacement(f32 t, f32 headU, f32 amplitude, f32 frequency,
                           f32 coherence, f32 edge, bool bothEnds) {
    const f32 e = (edge > 1e-6f) ? edge : 1.0f;
    f32 mute = (std::min)(t / e, 1.0f);
    if (bothEnds)
        mute *= (std::min)((1.0f - t) / e, 1.0f);
    const f32 u = t * frequency, w = coherence * headU;
    const f32 scale = amplitude * mute;
    return {NoiseField(u, w, 0.0f) * scale, NoiseField(u, w, 0.33f) * scale,
            NoiseField(u, w, 0.66f) * scale};
}

// --- Terrain collision (W4) --------------------------------------------------
// CRibbon_CollideSegment_Terrain's response, replayed against the grid. The
// engine sweeps a segment's step through the map colliders (radius 0.03,
// dword_103C2C9D0) and on contact reflects the post-integrate velocity off the
// hit surface as v' = -bounce·v_norm + friction·v_tan (friction only above a
// speed floor, dword_103C458F8 = 0.01), then advances from the contact point
// for the rest of the step. The ground query stands in for the colliders and
// answers a HEIGHT, so the surface is horizontal (normal = up); the dual
// query's forward-particle-system half has nothing to hit in the viewer.
// Positions and velocity are in the space the query answers (scene).
constexpr f32 kCollideRadius = 0.03f;
constexpr f32 kCollideSpeedSq = 0.01f;

struct GroundHit {
    Vector3f pos;
    Vector3f vel;
    bool hit = false;
};

GroundHit Sc2GroundCollide(const Vector3f& oldPos, const Vector3f& newPos,
                           const Vector3f& vel, f32 dt, f32 friction, f32 bounce,
                           const GroundQuery& query) {
    GroundHit r{newPos, vel, false};
    // Height under the step's end; a generous reach so a fast fall is not
    // missed (the flat grid ignores x/y, a host's terrain answers in range).
    constexpr f32 kReach = 1000.0f;
    f32 gz = 0.0f;
    if (!query(newPos, kReach, kReach, gz))
        return r;
    const f32 contactZ = gz + kCollideRadius;
    if (newPos.z > contactZ)
        return r; // ended above the surface: no contact this step.

    // Time of impact along the (z-monotone) step, then the contact point.
    const f32 dz = oldPos.z - newPos.z;
    const f32 frac =
        (dz > 1e-6f) ? std::clamp((oldPos.z - contactZ) / dz, 0.0f, 1.0f) : 0.0f;
    const Vector3f hit = {oldPos.x + (newPos.x - oldPos.x) * frac,
                          oldPos.y + (newPos.y - oldPos.y) * frac, contactZ};

    // Reflect only a velocity moving into the surface (dot(vel, up) < 0).
    const f32 vn = vel.z; // normal = grid up {0, 0, 1}
    if (vn >= 0.0f)
        return r;
    const Vector3f vNorm = {0.0f, 0.0f, vn};
    Vector3f vNew = {-bounce * vNorm.x, -bounce * vNorm.y, -bounce * vNorm.z};
    const f32 speedSq = vel.x * vel.x + vel.y * vel.y + vel.z * vel.z;
    if (speedSq > kCollideSpeedSq) {
        vNew.x += friction * (vel.x - vNorm.x);
        vNew.y += friction * (vel.y - vNorm.y);
        vNew.z += friction * (vel.z - vNorm.z);
    }
    // Advance in the binary's op order: (v'·dt)·(1 − frac), not v'·(dt·(1 − frac))
    // — f32 multiply is not associative, and O8 pins the binary's association.
    const f32 rem = 1.0f - frac;
    r.pos = {hit.x + (vNew.x * dt) * rem, hit.y + (vNew.y * dt) * rem,
             hit.z + (vNew.z * dt) * rem};
    r.vel = vNew;
    r.hit = true;
    return r;
}

} // namespace

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

u8 SelectSc2SimTechnique(const Sc2RibbonEmitterConfig& cfg) {
    // Bit-for-bit with the binary's decision order (oracle O1, 720 vectors):
    // ForceCPUSim wins outright; a spline is demoted to legacy only by noise;
    // forces/collide/UseLengthAndTime/noise force legacy; then accurate
    // tangents, then the length cull, then pure GPU.
    if (cfg.flags & 0x200)
        return 4;
    if (!cfg.splines.empty())
        return (cfg.noiseAmplitude > 0.001f) ? u8{4} : u8{1};
    if (cfg.forcesFallback != 0 || (cfg.flags & (0x2u | 0x4u | 0x1000u)) != 0 ||
        cfg.noiseAmplitude > 0.001f)
        return 4;
    if (cfg.flags & 0x2000)
        return 3;
    if (cfg.cullMethod == 1)
        return 2;
    return 0;
}

RibbonDesc DescFromSc2Config(const Sc2RibbonEmitterConfig& cfg) {
    RibbonDesc d;
    d.family = RibbonDesc::Family::Sc2;
    // The WC3-family scalars stay at defaults; the SC2 stages never read them.
    d.sc2.flags = cfg.flags;
    d.sc2.additionalFlags = cfg.additionalFlags;
    d.sc2.ribbonType = cfg.ribbonType;
    d.sc2.cullMethod = cfg.cullMethod;
    d.sc2.simTechnique = SelectSc2SimTechnique(cfg);
    d.sc2.divisions = cfg.divisions;
    d.sc2.edges = cfg.edges;
    d.sc2.innerRadius = cfg.innerRadius;
    for (i32 i = 0; i < 4; ++i) {
        // The ≤ 0.996 clamp is 5.0's load-time rule (RE §0); the mid-time is a
        // divisor in the VS two-piece interpolators, so 1.0 exactly would
        // divide by zero in the second piece.
        d.sc2.midTime[i] = (std::min)(cfg.midTime[i], 0.996f);
        d.sc2.midHold[i] = cfg.midHold[i];
    }
    d.sc2.sizeSmoothing = cfg.sizeSmoothing;
    d.sc2.colorSmoothing = cfg.colorSmoothing;
    d.sc2.drag = (std::max)(cfg.drag, 0.01f); // the engine's load-time floor
    d.sc2.mass = cfg.mass;
    d.sc2.gravity3 = cfg.gravity3;
    d.sc2.friction = cfg.friction;
    d.sc2.bounce = cfg.bounce;
    d.sc2.noiseAmplitude = cfg.noiseAmplitude;
    d.sc2.noiseFrequency = cfg.noiseFrequency;
    d.sc2.noiseCoherence = cfg.noiseCoherence;
    d.sc2.noiseEdge = cfg.noiseEdge;
    for (i32 i = 0; i < 5; ++i)
        d.sc2.waveTypes[i] = cfg.waveTypes[i];
    d.sc2.lodReduce = cfg.lodReduce;
    d.sc2.lodCut = cfg.lodCut;
    d.sc2.hasSpline = !cfg.splines.empty();
    if (d.sc2.hasSpline)
        d.sc2.spline = cfg.splines.front(); // splineData = splineRibbons.ptr[0]
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
    // A spline ribbon has no per-segment emission: Simulate_Spline rebuilds the
    // whole strip from four control points each frame, so it runs its own tick
    // instead of the emit/move stages (RE §3.4, RIBBON_SERVICE_PLAN.md W5).
    if (desc_.family == RibbonDesc::Family::Sc2 && desc_.sc2.hasSpline) {
        TickSc2Spline(dt);
        return;
    }

    // One tick = the family's stage variants. The WC3 family runs them in the
    // client's own order (retire → emit → move); the SC2 variants land phase
    // by phase and slot into these same switches.
    TickCtx t;
    switch (desc_.family) {
    case RibbonDesc::Family::Wc3:
        PrepWc3(t, dt);
        break;
    case RibbonDesc::Family::Sc2:
        PrepSc2(t, dt);
        break;
    }
    switch (desc_.family) {
    case RibbonDesc::Family::Wc3:
        RetireWc3(t);
        break;
    case RibbonDesc::Family::Sc2:
        break; // SC2 retires inside MoveSc2 (Simulate's deathU < headU walk).
    }
    switch (desc_.family) {
    case RibbonDesc::Family::Wc3:
        EmitWc3(t);
        break;
    case RibbonDesc::Family::Sc2:
        EmitSc2(t);
        break;
    }
    switch (desc_.family) {
    case RibbonDesc::Family::Wc3:
        MoveWc3(t);
        break;
    case RibbonDesc::Family::Sc2:
        MoveSc2(t);
        break;
    }
}

void RibbonEmitter::PrepWc3(TickCtx& t, f32 dt) {
    t.lifeSpan = SimLifespan();
    t.firstTick = !updatedOnce_;

    if (behavior_.firstFrameEmitsOneEdge && t.firstTick && desc_.edgesPerSecond > 0)
        dt = 1.0f / desc_.edgesPerSecond + 1e-4f;
    updatedOnce_ = true;

    if (behavior_.clampDtToLifespan) {
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

                RibbonElement e;
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
        headPending_ = behavior_.headEdgeIsProvisional;
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
        const f32 fall = behavior_.gravitySign * (g * dt * dt + 2.0f * g * e.age * dt);
        e.top.z += fall;
        e.bot.z += fall;
        e.age += dt;
    }
}

// ---------------------------------------------------------------------------
// SC2 time-mode stages. The per-element launch state is the oracle-gated
// kernel; the frame loop is RIBBON_SERVICE.md §5.1 (design-driven): headU is
// the emission-time clock (seconds), a segment is committed each
// emitPeriod = P / (emissionScale·lodKeepFactor·divisions) with the last two
// factors 1 at viewer quality, and Simulate_Type0 retires whatever has aged
// past its deathU. Length/Legacy techniques (2/3/4) are W4.
// ---------------------------------------------------------------------------

f32 RibbonEmitter::Sc2SegmentsPerSecond() const {
    // P = lifetime (time mode) / maxLength (length mode). rate = divisions / P
    // keeps `divisions` segments alive across one P.
    const f32 P = (desc_.sc2.cullMethod == 1) ? state_.sc2.maxLength
                                              : state_.sc2.lifetime;
    if (P <= 0.0f || desc_.sc2.divisions <= 0.0f)
        return 0.0f;
    return desc_.sc2.divisions / P;
}

RibbonElement RibbonEmitter::Sc2MakeSegment(f32 birthU, f32 fracToCurr) const {
    sc2::HeadInputs in;
    in.simTechnique = desc_.sc2.simTechnique;
    in.ribbonType = desc_.sc2.ribbonType;
    in.cullMethod = desc_.sc2.cullMethod;
    in.swapYawPitch = (desc_.sc2.flags & 0x8000u) != 0;
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
    for (i32 i = 0; i < 5; ++i) {
        in.waveTypes[i] = desc_.sc2.waveTypes[i];
        in.waveAmp[i] = state_.sc2.waveAmp[i];
        in.waveFreq[i] = state_.sc2.waveFreq[i];
    }
    in.overlayPhase = state_.sc2.overlayPhase;
    in.overlayTime = birthU;
    const sc2::HeadElement h = sc2::Sc2WriteHead(in);

    RibbonElement e;
    // Two anchoring modes, chosen by additionalFlags & 8. Nearly every shipped
    // hero ribbon is WORLD-space (the corruptor tentacles, the zealot hair):
    // the segment is born at the emitter's world position AT birth time and its
    // launch velocity/up are rotated into world, so the strip traces the
    // emitter's path through space plus the drag arc — that path IS the tentacle
    // spread. LOCAL-space (flag clear) births at the emitter origin and BUILD
    // transforms the whole strip by the current world matrix (a rigid drag jet).
    // Inherit parent velocity (flags & 0x10): the binary gates it on that flag
    // ALONE and folds the smoothed emitter velocity into the head velocity, then
    // tests the 1e-4 stationary floor (techs 0/2/3) on the POST-inherit vector,
    // replacing a degenerate one with the element-space direction·1e-4
    // (UpdateHeadSegment steps 4→5). Both spaces run the same order; the world
    // branch transforms first, the local branch adds the world smoothedDir to
    // the raw local velocity (the binary's own quirk — DRIFT-2 kept local inherit).
    const bool inherit = (desc_.sc2.flags & 0x10u) != 0;
    const bool floorTech = (in.simTechnique == 0 || in.simTechnique == 2 ||
                            in.simTechnique == 3);
    const f32 k = state_.sc2.parentVelocityScale;
    constexpr f32 kSqFloor = 1e-4f; // dword_103BB6B78
    if (desc_.sc2.additionalFlags & 0x8u) {
        // birthU sits between the previous and current pose; interpolate the
        // world origin the same fraction, like EmitWc3 interpolates its edges.
        e.birthPos = {prevPos_.x + (currPos_.x - prevPos_.x) * fracToCurr,
                      prevPos_.y + (currPos_.y - prevPos_.y) * fracToCurr,
                      prevPos_.z + (currPos_.z - prevPos_.z) * fracToCurr};
        const Vector3f dirW = whiteout::transform_normal(h.dir, state_.transform);
        e.velocity = whiteout::transform_normal(h.velocity, state_.transform);
        e.up = whiteout::transform_normal(h.up, state_.transform);
        if (inherit)
            e.velocity = {e.velocity.x + sc2SmoothedVel_.x * k,
                          e.velocity.y + sc2SmoothedVel_.y * k,
                          e.velocity.z + sc2SmoothedVel_.z * k};
        if (floorTech) {
            const f32 sq = (e.velocity.x * e.velocity.x + e.velocity.y * e.velocity.y) +
                           e.velocity.z * e.velocity.z;
            if (sq < kSqFloor)
                e.velocity = {dirW.x * kSqFloor, dirW.y * kSqFloor, dirW.z * kSqFloor};
        }
    } else {
        (void)fracToCurr;
        e.birthPos = {0, 0, 0};
        e.velocity = h.velocity;
        e.up = h.up;
        if (inherit)
            e.velocity = {e.velocity.x + sc2SmoothedVel_.x * k,
                          e.velocity.y + sc2SmoothedVel_.y * k,
                          e.velocity.z + sc2SmoothedVel_.z * k};
        if (floorTech) {
            const f32 sq = (e.velocity.x * e.velocity.x + e.velocity.y * e.velocity.y) +
                           e.velocity.z * e.velocity.z;
            if (sq < kSqFloor)
                e.velocity = {h.dir.x * kSqFloor, h.dir.y * kSqFloor, h.dir.z * kSqFloor};
        }
    }
    e.pos = e.birthPos;
    e.size3 = h.size3;
    e.rotation3 = h.rotation3;
    e.invMass = h.invMass;
    e.birthU = h.birthU;
    e.deathU = h.deathU;
    // Colour stops, with the alpha overlay wave added and clamped (the head
    // kernel hands the wave back rather than owning the colours).
    for (i32 i = 0; i < 3; ++i) {
        e.color3[i] = state_.sc2.color3[i];
        e.color3[i].w = std::clamp(e.color3[i].w + h.alphaWave, 0.0f, 1.0f);
    }
    return e;
}

void RibbonEmitter::CommitSc2Segment(f32 birthU, f32 fracToCurr) {
    edges_.push_back(Sc2MakeSegment(birthU, fracToCurr));
}

void RibbonEmitter::PrepSc2(TickCtx& t, f32 dt) {
    t.lifeSpan = state_.sc2.lifetime;
    t.firstTick = !updatedOnce_;
    updatedOnce_ = true;
    t.dt = (dt >= 0) ? dt : 0.0f;

    // Inherit-parent-velocity smoothing: push this tick's emitter motion into
    // the 8-tap ring (no-op unless inheriting + world-space).
    Sc2UpdateSmoothedVelocity(t.dt);

    // Spawn pre-roll (CatchUpEmission): the first tick after seeding lays the
    // trail's age distribution so it does not pop in truncated. Without motion
    // history the pre-rolled segments sit at the spawn pose; their staggered
    // birthU/deathU is the part that matters. The tick count is the gated O5
    // kernel; each pre-roll tick is a full 33 ms emit + (legacy) move + retire.
    if (posSet_ && !sc2CaughtUp_ && state_.sc2.active) {
        sc2CaughtUp_ = true;
        const sc2::CatchUpResult cu = sc2::Sc2CatchUpTicks(
            desc_.sc2.cullMethod, state_.sc2.speed, state_.sc2.lifetime,
            state_.sc2.maxLength);
        if (!cu.earlyOut) {
            for (i32 k = 0; k < cu.ticks; ++k) {
                Sc2Append(0.033f);
                if (desc_.sc2.simTechnique == 4)
                    Sc2LegacyIntegrate(0.033f);
                Sc2Retire();
            }
        }
    }
}

void RibbonEmitter::Sc2Append(f32 dt) {
    const bool emitting = posSet_ && IsEmitterVisible(state_.visibility) &&
                          state_.sc2.active;
    if (emitting) {
        const f32 rate = Sc2SegmentsPerSecond();
        const f32 endCount = sc2EmitAccum_ + dt * rate;
        const i32 numNew = static_cast<i32>(floorf(endCount));
        const f32 span = endCount - sc2EmitAccum_;
        const f32 ooSpan = (span > kVectorEpsilon) ? 1.0f / span : 1.0f;
        for (i32 i = 0; i < numNew; ++i) {
            const f32 lap = static_cast<f32>(i + 1) - sc2EmitAccum_;
            const f32 frac = std::clamp(lap * ooSpan, 0.0f, 1.0f);
            CommitSc2Segment(sc2HeadU_ + frac * dt, frac);
        }
        sc2EmitAccum_ = endCount - floorf(endCount);
    }
    // headU is the clock whether or not we emit, so a muted trail still ages
    // out (active gates NEW segments, never the live trail).
    sc2HeadU_ += dt;
}

void RibbonEmitter::Sc2Retire() {
    // Segments are in birth order, oldest at the front. Retire from the front
    // whatever has aged past its deathU (Simulate_Type0's deathU < headU walk).
    const f32 headU = sc2HeadU_;
    edges_.erase(edges_.begin(),
                 std::find_if(edges_.begin(), edges_.end(),
                              [headU](const RibbonElement& e) {
                                  return e.deathU >= headU;
                              }));
}

void RibbonEmitter::EmitSc2(TickCtx& t) {
    Sc2Append(t.dt);
}

void RibbonEmitter::MoveSc2(const TickCtx& t) {
    // Legacy (tech 4) integrates each segment's stored velocity/position under
    // gravity + drag before retiring; the analytic techniques (0/2/3) leave the
    // motion to the closed form the VS/BUILD applies (Simulate_Type0 is
    // append + retire only).
    if (desc_.sc2.simTechnique == 4)
        Sc2LegacyIntegrate(t.dt);
    Sc2Retire();
}

void RibbonEmitter::Sc2LegacyIntegrate(f32 dt) {
    if (dt <= 0.0f)
        return;
    const auto& s = desc_.sc2;
    const bool worldSpace = (s.additionalFlags & 0x8u) != 0;
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
    const bool collide = static_cast<bool>(state_.groundQuery) && (s.flags & 0x2u) != 0;
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
                Sc2GroundCollide(oS, nS, vS, dt, s.friction, s.bounce, state_.groundQuery);
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

void RibbonEmitter::Sc2UpdateSmoothedVelocity(f32 dt) {
    // The smoothing ring is fed the world-matrix translation delta whenever
    // inherit (flags & 0x10) is set — EmitSegments' gate (0x102953761) does NOT
    // test world/local, so a LOCAL + inherit ribbon also accumulates a nonzero
    // smoothedDir (DRIFT-2). Only the head ELEMENT position is forced 0 for local
    // (A2); the marched world headPos/smoothedDir are not — the old "local
    // smoothedDir is always 0" was wrong.
    if ((desc_.sc2.flags & 0x10u) == 0)
        return;
    if (!posSet_ || dt <= 0.0f)
        return;
    sc2SmoothPos_[sc2SmoothSlot_] = {currPos_.x - prevPos_.x, currPos_.y - prevPos_.y,
                                     currPos_.z - prevPos_.z};
    sc2SmoothDt_[sc2SmoothSlot_] = dt;
    sc2SmoothSlot_ = static_cast<u8>((sc2SmoothSlot_ + 1) & 7);
    if (sc2SmoothCount_ < 8)
        ++sc2SmoothCount_;
    // smoothedVel = Σ posDelta / Σ dt over the window: the dt-weighted average
    // emitter velocity (units/s).
    Vector3f sum = {0, 0, 0};
    f32 wsum = 0;
    for (u8 i = 0; i < sc2SmoothCount_; ++i) {
        sum = {sum.x + sc2SmoothPos_[i].x, sum.y + sc2SmoothPos_[i].y,
               sum.z + sc2SmoothPos_[i].z};
        wsum += sc2SmoothDt_[i];
    }
    sc2SmoothedVel_ = (wsum > 1e-6f)
                          ? Vector3f{sum.x / wsum, sum.y / wsum, sum.z / wsum}
                          : Vector3f{0, 0, 0};
}

i32 RibbonEmitter::VertexCount() const {
    const i32 n = (i32)edges_.size();
    return (n > 1) ? (n - 1) * 6 : 0;
}

i32 RibbonEmitter::BuildStage(const RibbonBuildContext& ctx, std::vector<Vertex>& out,
                              std::vector<RibbonDrawList>& outDraws) const {
    switch (desc_.family) {
    case RibbonDesc::Family::Sc2: {
        const i32 sc2Off = (i32)out.size();
        const i32 sc2Added =
            desc_.sc2.hasSpline ? BuildStripSc2Spline(ctx, out) : BuildStripSc2(ctx, out);
        if (sc2Added <= 0)
            return 0;
        // One draw record, routed through the resolved M3 surface (blend +
        // lighting). m3Surface == -1 falls back to the BLS path.
        RibbonDrawList dl;
        dl.model = 0;
        dl.emitterId = 0;
        dl.vertexOffset = sc2Off;
        dl.vertexCount = sc2Added;
        dl.priorityPlane = desc_.priorityPlane;
        dl.m3Surface = desc_.sc2.m3Surface;
        dl.twoSided = true;
        dl.worldOrigin = out[(usize)sc2Off].position;
        outDraws.push_back(dl);
        return sc2Added;
    }
    case RibbonDesc::Family::Wc3:
        break;
    }
    const i32 offset = (i32)out.size();
    const i32 added = BuildStrip(out);
    if (added <= 0)
        return 0;

    // One strip, drawn once per layer. `CRibbonEmitter::Render` @0x100e7e1d0
    // loops the emitter's CRibbonMat array rebinding texture and blend state
    // each pass over the SAME vertex/index buffer, so the passes share a
    // vertex range and differ only in material. Order is the array's, which
    // the transparent queue preserves via the unit index.
    const Vector3f origin = out[(usize)offset].position;
    for (const RibbonLayer& layer : desc_.layers) {
        RibbonDrawList dl;
        dl.vertexOffset = offset;
        dl.vertexCount = added;
        dl.priorityPlane = desc_.priorityPlane;
        dl.textureId = layer.textureId;
        dl.filterMode = layer.filterMode;
        dl.unshaded = layer.unshaded;
        dl.twoSided = layer.twoSided;
        dl.worldOrigin = origin;
        outDraws.push_back(dl);
    }
    return added;
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

i32 RibbonEmitter::BuildStripSc2(const RibbonBuildContext& ctx,
                                 std::vector<Vertex>& out) const {
    if (state_.visibility <= 0.0f || edges_.size() < 2)
        return 0;

    namespace vs = whiteout::flakes::renderer::ribbon::vs;
    const auto& s = desc_.sc2;
    const Matrix44f& xform = state_.transform;
    const f32 mass = (s.mass > 0.0f) ? s.mass : 1.0f;
    const f32 drag = (s.drag > 0.0f) ? s.drag : 0.01f;
    const f32 gravityZ = s.gravity3.z;
    const bool worldSpace = (s.additionalFlags & 0x8u) != 0;
    const u8 tech = s.simTechnique;
    // Tangent source (Ribbon.fx:409/446): the analytic velocity for the pure
    // procedural techniques (0 GPUONLY, 2 MIXED); the trail geometry for the
    // accurate-tangent (3) and legacy (4) ones, which retail precomputes on
    // the CPU. -instVel degenerates to {1,0,0} for a slow world-trail, so a
    // tube built from it collapses to a line — the geometry tangent is what
    // makes a world-space ribbon read as a spreading trail.
    const bool velTangent = (tech == 0 || tech == 2);
    // Ribbon.fx:457 — the non-flattened ("smooth") frame branch; every
    // technique but the pure billboard (0) takes it.
    const bool smoothPath = (tech == 2 || tech == 3 || tech == 4);
    const int xsec = s.ribbonType; // 0 billboard / 1 planar / 2 cylinder / 3 star
    const bool tube = (xsec == 2 || xsec == 3);
    // Cylinder = edges ring verts; star = 2·edges (one inner + one outer per
    // authored edge, BuildCrossSection type 3).
    const int baseEdges = tube ? std::clamp(s.edges, 3, 64) : 1;
    const int ringEdges = (xsec == 3) ? baseEdges * 2 : baseEdges;
    const Vector3f camDir = ctx.cameraDir;

    // midTime[] is size/color/alpha/rotation; a 0 divisor would blow up the
    // interpolators, so clamp the reciprocal's input the way the desc does.
    auto invMid = [](f32 m) { return (m > 0.0f) ? 1.0f / m : 1.0f; };

    // Per element: displaced world position, then (once fAge is known) the
    // interpolated scalars. `src` keeps the element so the interpolation pass
    // can resample — length mode reparameterises fAge by arc length, so the
    // scalars can't be sampled until the strip is measured and trimmed.
    struct Node {
        Vector3f pos, tangent, up;
        f32 fAge, twist, size, v;
        Vector4f color;
        const RibbonElement* src;
    };
    std::vector<Node> nodes;
    nodes.reserve(edges_.size() + 1);

    const bool legacy = (tech == 4);
    // Legacy (tech 4): position/velocity were integrated per-tick and stored on
    // the element (Simulate_Type4), so BUILD reads them directly — the VS sets
    // b_proceduralPosition = false for tech 4. The analytic techniques (0/2/3)
    // reconstruct pos = birthPos + closed-form drag/gravity displacement(age)
    // here (the VS's b_proceduralPosition path). Age is the RAW seconds
    // (ribbon_vs.slang:402); gravity takes the renderer-unit scale in world space
    // (the velocity was already scaled into world). `e` must outlive `nodes`.
    auto appendNode = [&](const RibbonElement& e) {
        if (e.deathU - e.birthU <= 0.0f)
            return;
        Vector3f localPos, localVel;
        if (legacy) {
            localPos = e.pos;
            localVel = e.velocity;
        } else {
            const f32 age = sc2HeadU_ - e.birthU;
            const f32 g = worldSpace ? (-gravityZ * state_.unitScale) : -gravityZ;
            const vs::DragResult d = vs::CalculateDisplacementAndVelocity(
                age, e.velocity, mass, 1.0f / mass, drag, 1.0f / drag, g);
            localPos = vs::Add(e.birthPos, d.displacement);
            localVel = d.velocity;
        }
        Node n;
        if (worldSpace) {
            n.pos = localPos;
            n.tangent = vs::Scale(localVel, -1.0f); // provisional (velTangent)
            n.up = e.up;
        } else {
            n.pos = whiteout::transform_point(localPos, xform);
            n.tangent = whiteout::transform_normal(vs::Scale(localVel, -1.0f), xform);
            n.up = whiteout::transform_normal(e.up, xform);
        }
        n.up = vs::SafeNormalize(n.up, Vector3f{0, 0, 1});
        n.src = &e;
        nodes.push_back(n);
    };
    for (const RibbonElement& e : edges_)
        appendNode(e);
    // Live head. The binary keeps a persistent head element (pBuffer[3]) that
    // CRibbon_UpdateHeadSegment re-stamps EVERY frame at the emitter world pos
    // with birthU == headU, and Simulate skips integrating it. Our edges_ hold
    // only the frozen history, appended at the sub-frame emission cadence
    // (divisions/maxLength, ~one segment per 8 frames for Tyrael's wings), so
    // without the head the newest strip vertex is a discrete spawn: arcFront —
    // hence rpVScale, hence every node's V→size/twist — jumps on each spawn and
    // retire, the Tyrael-wing / Zealot-hair twinkle at rest. Synthesising the
    // head here anchors the leading edge to the emitter (birthU == headU keeps
    // the newest age at 0), so the arc grows continuously between spawns.
    const RibbonElement liveHead = Sc2MakeSegment(sc2HeadU_, 1.0f);
    appendNode(liveHead);
    if (nodes.size() < 2)
        return 0;

    // Geometry tangents for the accurate/legacy techniques: the averaged
    // direction of the two adjacent segments (Ribbon.fx:446), one-sided at the
    // ends. The procedural techniques keep the analytic velocity above.
    if (!velTangent) {
        std::vector<Vector3f> tan(nodes.size());
        for (usize i = 0; i < nodes.size(); ++i) {
            if (i == 0)
                tan[i] = vs::Sub(nodes[1].pos, nodes[0].pos);
            else if (i + 1 == nodes.size())
                tan[i] = vs::Sub(nodes[i].pos, nodes[i - 1].pos);
            else
                tan[i] = vs::Sub(nodes[i + 1].pos, nodes[i - 1].pos);
        }
        for (usize i = 0; i < nodes.size(); ++i)
            nodes[i].tangent = tan[i];
    }
    for (Node& n : nodes)
        n.tangent = vs::SafeNormalize(n.tangent, Vector3f{1, 0, 0});

    // fAge per node. Time mode (cullMethod 0): the age fraction over the
    // segment's own lifespan (Ribbon.fx:387). Length mode (cullMethod 1): the
    // strip is culled to `maxLength` of centreline for GEOMETRY, and V is
    // ARC-based, not an age proxy (RIBBON_REVIEW_FINDINGS A4/A9/DRIFT-3, from
    // Simulate_Type4 0x1029604A0 phase 2 and Simulate_Type2 0x10295F520):
    //   tech 4 (legacy):    V = arcFromHead / (unitScale·maxLength), per segment;
    //   tech 2/3 (analytic): V = age · rpVScale, rpVScale = 1/(headU − cutU),
    //     cut = the birthU interpolated at the arc = maxLength point.
    // The arc is measured in render space, so unitScale cancels against maxLen —
    // the denominator is the AUTHORED length (stable), so V does not pulse with
    // the flapping geometry (the earlier speed/maxLength age-proxy dropped the
    // worldScale factor and mis-stretched under variable emitter speed).
    // flags & 0x1000 (UseLengthAndTime) maxes V with the time fraction. Nodes run
    // oldest (front) → head (back).
    const bool lengthMode = (s.cullMethod == 1);
    const f32 maxLen = state_.sc2.maxLength * state_.unitScale;
    if (lengthMode && maxLen > 0.0f) {
        const usize head = nodes.size() - 1;
        std::vector<f32> arcFromHead(nodes.size(), 0.0f); // render-space arc head→node
        f32 acc = 0.0f;
        usize keepFrom = 0;
        bool cutFound = false;
        f32 cutU = nodes.front().src->birthU; // oldest kept birthU (no-cut default)
        for (usize i = head; i > 0; --i) {
            const f32 seg = vs::Length3(vs::Sub(nodes[i - 1].pos, nodes[i].pos));
            if (acc + seg >= maxLen && seg > 1e-6f) {
                const f32 t = (maxLen - acc) / seg; // land the tail on maxLength
                nodes[i - 1].pos = vs::Lerp(nodes[i].pos, nodes[i - 1].pos, t);
                arcFromHead[i - 1] = maxLen;
                cutU = nodes[i].src->birthU +
                       (nodes[i - 1].src->birthU - nodes[i].src->birthU) * t;
                keepFrom = i - 1;
                cutFound = true;
                break;
            }
            acc += seg;
            arcFromHead[i - 1] = acc;
        }
        if (keepFrom > 0) {
            const auto cut = static_cast<std::ptrdiff_t>(keepFrom);
            nodes.erase(nodes.begin(), nodes.begin() + cut);
            arcFromHead.erase(arcFromHead.begin(), arcFromHead.begin() + cut);
        }
        // rpVScale (Simulate_Type2/3 0x10295F520/0x10295FBE0 phase-2 arc walk):
        // V spreads the arc fraction over the age span from the head. A cut
        // anchors it at headU→cutU (V reaches 1 at the cut); an UNCUT trail
        // (arc < maxLength, incl. at rest) reaches only arc/maxLength at the
        // oldest. The binary writes uncut = v45/(v72−v41) with v72 = the newest
        // element's birthU and v41 = the oldest's. The newest element is the
        // persistent head (pBuffer[3]) that UpdateHeadSegment re-stamps every
        // frame at birthU==headU, so v72==headU; the synthesised live head above
        // is that element, so headU−cutU (cutU = the oldest birthU here) is
        // exactly v72−v41. cutFound mirrors the binary's cut flag — any element
        // whose cumulative arc reaches maxLength, including one landing on the
        // oldest node (keepFrom 0 but cutFound).
        const f32 headU = sc2HeadU_;
        const f32 span = headU - cutU;
        const f32 rpVScale =
            (span <= 1e-6f) ? 0.0f
            : cutFound      ? 1.0f / span
                            : (arcFromHead.front() / maxLen) / span;
        const bool useLengthAndTime = (s.flags & 0x1000u) != 0;
        for (usize i = 0; i < nodes.size(); ++i) {
            f32 v = legacy ? (arcFromHead[i] / maxLen)                    // tech 4
                           : (headU - nodes[i].src->birthU) * rpVScale;   // tech 2/3
            if (useLengthAndTime)
                v = (std::max)(v, vs::FAge(headU, nodes[i].src->birthU,
                                           nodes[i].src->deathU, 1.0f));
            nodes[i].fAge = (std::min)(v, 1.0f);
        }
    } else {
        for (Node& n : nodes)
            n.fAge = vs::FAge(sc2HeadU_, n.src->birthU, n.src->deathU, 1.0f);
    }

    // Interpolated scalars, now that fAge is settled. The sampled size is model
    // units; the half-width is added in WORLD space, so it takes the
    // model->renderer factor by hand (as EmitWc3 rescales above/below).
    // fSize *= 0.5 mirrors Ribbon.fx:442.
    for (Node& n : nodes) {
        const RibbonElement& e = *n.src;
        n.twist = vs::TwistAngle(n.fAge, e.rotation3.x, e.rotation3.y, e.rotation3.z,
                                 (s.midTime[3] > 0.0f) ? s.midTime[3] : 0.5f);
        const f32 fSize = vs::InterpolateValue(n.fAge, e.size3.x, e.size3.y, e.size3.z,
                                               s.midTime[0], invMid(s.midTime[0]),
                                               s.midHold[0], s.sizeSmoothing);
        n.size = fSize * 0.5f * state_.unitScale;
        const Vector3f rgb = vs::InterpolateValue3(
            n.fAge, {e.color3[0].x, e.color3[0].y, e.color3[0].z},
            {e.color3[1].x, e.color3[1].y, e.color3[1].z},
            {e.color3[2].x, e.color3[2].y, e.color3[2].z}, s.midTime[1],
            invMid(s.midTime[1]), s.midHold[1], s.colorSmoothing);
        const f32 a = vs::InterpolateValue(n.fAge, e.color3[0].w, e.color3[1].w,
                                           e.color3[2].w, s.midTime[2],
                                           invMid(s.midTime[2]), s.midHold[2],
                                           s.colorSmoothing);
        n.color = {rgb.x, rgb.y, rgb.z, a};
        n.v = n.fAge; // V = fAge (RE §4.6)

        // Noise displacement (RE §4.3): a per-node positional wobble, present
        // only for legacy ribbons that authored noise (noise > 0.001 forces
        // tech 4). n.pos is in render space, so the amplitude takes the
        // model→renderer factor; muted toward the head only (standard ribbons).
        if (legacy && s.noiseAmplitude > 0.001f) {
            n.pos = vs::Add(n.pos, NoiseDisplacement(
                                       n.fAge, sc2HeadU_,
                                       s.noiseAmplitude * state_.unitScale,
                                       s.noiseFrequency, s.noiseCoherence,
                                       s.noiseEdge, /*bothEnds=*/false));
        }
    }

    // U across the width (0/1), V = fAge along the length (the SC2 clock puts
    // age on V, RE §4.6); the material's own tiling is separate. The section
    // expansion is shared with the spline path.
    std::vector<StripNode> strip;
    strip.reserve(nodes.size());
    for (const Node& n : nodes)
        strip.push_back({n.pos, n.tangent, n.up, n.size, n.twist, n.v, n.color});
    return EmitSc2Strip(strip, xsec, ringEdges, tube, smoothPath, camDir,
                        s.innerRadius, out);
}

// ---------------------------------------------------------------------------
// SC2 spline ribbons (technique 1 / CPU spline, RIBBON_SERVICE_PLAN.md W5).
// One cubic Bezier from the single SRIB record — a whole-ribbon shape rebuilt
// each frame, not a trail of aged segments. Verified against CRibbon_Simulate_
// Spline (4.8 0x10295E170): control-point construction, persistent age² sag,
// 32 samples at t = i/31, up = world +Z, V = 1 − t.
// ---------------------------------------------------------------------------

void RibbonEmitter::TickSc2Spline(f32 dt) {
    updatedOnce_ = true;
    sc2SplineAge_ += (dt >= 0) ? dt : 0.0f;

    // Whole-ribbon death: past its lifetime the spline restarts (age and sag
    // reset), so a gravity-drooping spline loops rather than sagging forever
    // (RE §3.4: per-segment aging does not exist for splines).
    const f32 life = state_.sc2.lifetime;
    if (life > 0.0f && sc2SplineAge_ >= life) {
        sc2SplineAge_ = 0.0f;
        for (Vector3f& sg : sc2Sag_)
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
    const f32 age2 = sc2SplineAge_ * sc2SplineAge_ * state_.unitScale;
    const Vector3f d = {g.x * age2, g.y * age2, g.z * age2};
    for (Vector3f& sg : sc2Sag_)
        sg = d;
}

i32 RibbonEmitter::BuildStripSc2Spline(const RibbonBuildContext& ctx,
                                       std::vector<Vertex>& out) const {
    if (state_.visibility <= 0.0f)
        return 0;
    const auto& s = desc_.sc2;
    const auto& sp = s.spline;
    const bool swap = (s.flags & 0x8000u) != 0;

    // Control points in WORLD space. The binary builds them emitter-local and
    // multiplies by the emitter world at draw; folding inv(emitterWorld)·
    // emitterWorld away lets C0/C1 come straight off the emitter frame and
    // C3/C2 off the SRIB node frame, so no matrix inverse is needed.
    const Matrix44f& ew = state_.transform;
    const Matrix44f& nw = state_.sc2.splineNodeTransform;

    // Overlay waves (W6). Simulate_Spline runs the wave clock on the whole-ribbon
    // age; a channel with a wave type REPLACES the base yaw/pitch (its else
    // branch reads the static value only when the type is 0), while the
    // speed/velocity waves ADD to the tangent-length factors (scaled by the
    // precomputed norm factors). All inert when the types are 0, so a spline
    // with no wave channels is byte-identical to the W5 base.
    const f32 ot = sc2SplineAge_;
    const f32 phase = state_.sc2.overlayPhase;
    const u32* wt = desc_.sc2.waveTypes;      // main: yaw/pitch/speed/size/alpha
    const u32* swt = sp.waveTypes;            // spline: yaw/pitch/velocity
    const auto mainWave = [&](int i) -> f32 {
        return wt[i] ? sc2::Sc2SampleWave(wt[i], state_.sc2.waveFreq[i] * ot + phase,
                                          state_.sc2.waveAmp[i])
                     : 0.0f;
    };
    const auto splWave = [&](int i) -> f32 {
        return swt[i] ? sc2::Sc2SampleWave(swt[i], state_.sc2.splineWaveFreq[i] * ot + phase,
                                           state_.sc2.splineWaveAmp[i])
                      : 0.0f;
    };
    const f32 ribYaw = wt[0] ? mainWave(0) : state_.sc2.yawDeg;
    const f32 ribPitch = wt[1] ? mainWave(1) : state_.sc2.pitchDeg;
    const f32 sribYaw = swt[0] ? splWave(0) : state_.sc2.splineYawDeg;
    const f32 sribPitch = swt[1] ? splWave(1) : state_.sc2.splinePitchDeg;
    const vs::Mat3 rRib = YawPitchMat(ribYaw, ribPitch, swap);
    const vs::Mat3 rSrib = YawPitchMat(sribYaw, sribPitch, swap);
    const f32 baseFactor =
        state_.sc2.velocityBaseFactor + mainWave(2) * sp.emissionVectorNormFactor;
    const f32 endFactor =
        state_.sc2.velocityEndFactor + splWave(2) * sp.velocityNormFactor;
    const f32 sizeWave = mainWave(3);
    const f32 alphaWave = mainWave(4);

    // The four SRIB vec3s are DIRECT Bezier control points, each a POINT in its
    // own frame — Simulate_Spline (4.8 0x10295E170, pinned by O9) does NOT add
    // the endpoints to the tangents. C0/C1 are emissionOffset and the
    // RIB-rotated emissionVector·baseFactor, both through the emitter frame; C3/
    // C2 are endOffset and the SRIB-rotated endTangent·endFactor, both through
    // the SRIB node frame. C1 and C2 are transform_POINT (they take the frame's
    // translation too — O9's node-translate vector records C2 = nodeTranslate +
    // R_srib·endTangent, matching C3's translation with no cross term). Adding
    // the endpoints to the tangents was a documented-but-wrong RE guess (§3.4).
    const Vector3f c0 = whiteout::transform_point(sp.emissionOffset, ew);
    const Vector3f c1 = whiteout::transform_point(
        vs::Scale(vs::MulVecMat3(sp.emissionVector, rRib), baseFactor), ew);
    const Vector3f c3 = whiteout::transform_point(
        vs::MulVecMat3(sp.endOffset, rSrib), nw);
    const Vector3f c2 = whiteout::transform_point(
        vs::Scale(vs::MulVecMat3(sp.endTangent, rSrib), endFactor), nw);

    const Vector3f p0 = vs::Add(c0, sc2Sag_[0]);
    const Vector3f p1 = vs::Add(c1, sc2Sag_[1]);
    const Vector3f p2 = vs::Add(c2, sc2Sag_[2]);
    const Vector3f p3 = vs::Add(c3, sc2Sag_[3]);

    // Cross-section up. A GPU spline (no noise) computes a STABILIZED up in the
    // VS from the control points — the mean plane normal, x-biased so near-
    // colinear points still resolve (Ribbon.fx:361, pinned by O12's spline_up).
    // A fixed +Z twists a tube whenever the curve runs parallel to it (the
    // vertical Spine Crawler stalk). A noisy CPU spline (tech 4) instead writes
    // emitter +Z per element (RE §3.4), so keep that for the noise path.
    Vector3f splineUp;
    if (s.noiseAmplitude > 0.001f) {
        splineUp = vs::SafeNormalize(whiteout::transform_normal(Vector3f{0, 0, 1}, ew),
                                     Vector3f{0, 0, 1});
    } else {
        Vector3f n0 = vs::Cross3(vs::Sub(p1, p0), vs::Sub(p3, p0));
        const Vector3f n1 = vs::Cross3(vs::Sub(p2, p3), vs::Sub(p0, p3));
        if (vs::Dot3(n0, n1) < 0.0f)
            n0 = vs::Scale(n0, -1.0f);
        n0 = vs::Add(n0, n1);
        n0.x += 1.0f;
        splineUp = vs::SafeNormalize(n0, Vector3f{0, 0, 1});
    }

    // B(t) = C0(1−t)³ + 3C1·t(1−t)² + 3C2·t²(1−t) + C3·t³ (weights in the
    // binary's order).
    auto cubic = [&](f32 t) -> Vector3f {
        const f32 u = 1.0f - t;
        const f32 w0 = u * u * u, w1 = 3.0f * t * u * u, w2 = 3.0f * t * t * u,
                  w3 = t * t * t;
        return {p0.x * w0 + p1.x * w1 + p2.x * w2 + p3.x * w3,
                p0.y * w0 + p1.y * w1 + p2.y * w2 + p3.y * w3,
                p0.z * w0 + p1.z * w1 + p2.z * w2 + p3.z * w3};
    };

    constexpr int kSamples = 32; // t = i/31 (RE §3.4)
    const int xsec = s.ribbonType;
    const bool tube = (xsec == 2 || xsec == 3);
    const int baseEdges = tube ? std::clamp(s.edges, 3, 64) : 1;
    const int ringEdges = (xsec == 3) ? baseEdges * 2 : baseEdges; // star = 2·edges
    auto invMid = [](f32 m) { return (m > 0.0f) ? 1.0f / m : 1.0f; };

    std::vector<Vector3f> pos(kSamples);
    for (int i = 0; i < kSamples; ++i)
        pos[static_cast<usize>(i)] = cubic(static_cast<f32>(i) / (kSamples - 1.0f));

    // fAge = t drives the interpolators (start value at C0/emitter, end value at
    // C3/tip); V = 1 − t (RE §4.6). up = world +Z; tangent = the finite-diff
    // curve direction (spline elements carry no velocity, so the frame reads the
    // geometry). Every spline takes Ribbon.fx's smooth (non-flattened) branch.
    std::vector<StripNode> strip(kSamples);
    for (int i = 0; i < kSamples; ++i) {
        const usize u = static_cast<usize>(i);
        const f32 t = static_cast<f32>(i) / (kSamples - 1.0f);
        Vector3f tan;
        if (i == 0)
            tan = vs::Sub(pos[1], pos[0]);
        else if (i + 1 == kSamples)
            tan = vs::Sub(pos[u], pos[u - 1]);
        else
            tan = vs::Sub(pos[u + 1], pos[u - 1]);

        StripNode n;
        n.pos = pos[u];
        // Noise displacement (RE §4.3): splines mute BOTH ends (t/edge at the
        // head, (1−t)/edge at the tip). Present only when noise was authored
        // (which demotes the spline to tech 4 but keeps the spline geometry).
        if (s.noiseAmplitude > 0.001f) {
            n.pos = vs::Add(n.pos, NoiseDisplacement(
                                       t, sc2SplineAge_, s.noiseAmplitude * state_.unitScale,
                                       s.noiseFrequency, s.noiseCoherence, s.noiseEdge,
                                       /*bothEnds=*/true));
        }
        n.up = splineUp;
        n.tangent = vs::SafeNormalize(tan, Vector3f{1, 0, 0});
        n.twist = vs::TwistAngle(t, state_.sc2.rotation3.x, state_.sc2.rotation3.y,
                                 state_.sc2.rotation3.z,
                                 (s.midTime[3] > 0.0f) ? s.midTime[3] : 0.5f);
        // Size/alpha overlay waves add to the sampled size keys and to the
        // interpolated alpha (clamped), as Simulate_Spline does.
        const auto& col = state_.sc2.color3;
        const f32 fSize = vs::InterpolateValue(t, state_.sc2.size3.x + sizeWave,
                                               state_.sc2.size3.y + sizeWave,
                                               state_.sc2.size3.z + sizeWave, s.midTime[0],
                                               invMid(s.midTime[0]), s.midHold[0],
                                               s.sizeSmoothing);
        // Half-extent is size·0.25 (RE §4, A7): the binary halves TWICE — the
        // spline batch consts store half-sizes (or the tech-4 filler writes ·0.5)
        // and the VS halves again (Ribbon.fx:442). The trail path gets both (head
        // writer + BuildStripSc2); the spline path has no head writer, so it must
        // apply both here or the tube renders twice as thick.
        n.size = fSize * 0.25f * state_.unitScale;
        const Vector3f rgb = vs::InterpolateValue3(
            t, {col[0].x, col[0].y, col[0].z}, {col[1].x, col[1].y, col[1].z},
            {col[2].x, col[2].y, col[2].z}, s.midTime[1], invMid(s.midTime[1]),
            s.midHold[1], s.colorSmoothing);
        const f32 a = vs::InterpolateValue(t, col[0].w, col[1].w, col[2].w, s.midTime[2],
                                           invMid(s.midTime[2]), s.midHold[2],
                                           s.colorSmoothing);
        n.color = {rgb.x, rgb.y, rgb.z, std::clamp(a + alphaWave, 0.0f, 1.0f)};
        n.v = 1.0f - t;
        strip[u] = n;
    }
    return EmitSc2Strip(strip, xsec, ringEdges, tube, /*smoothPath=*/true,
                        ctx.cameraDir, s.innerRadius, out);
}

} // namespace whiteout::flakes::renderer::ribbon
