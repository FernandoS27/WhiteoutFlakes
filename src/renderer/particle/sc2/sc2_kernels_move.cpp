#include "renderer/particle/sc2/sc2_kernels_move.h"

#include "renderer/particle/sc2/sc2_emitter_desc.h"
#include "renderer/particle/sc2/sc2_kernel_math.h"

#include "renderer/sc2/sc2_element.h"
#include "renderer/sc2/sc2_element_math.h"

#include <algorithm>
#include <bit>
#include <cmath>

namespace whiteout::flakes::renderer::particle::sc2 {

namespace {

using detail::kFreezeSpeedSq;

} // namespace

// ===========================================================================
// MOVE (analytic) + RETIRE.
// ===========================================================================

AnalyticStep StepAnalytic(const AnalyticInputs& in) {
    namespace vs = whiteout::flakes::renderer::sc2::vs;

    AnalyticStep out;
    out.age = vs::Saturate((in.systemTime - in.birthTime) /
                           (in.deathTime - in.birthTime));

    // `Bug 187607: no negative time skew`, in the shader's own comment. A
    // batch clock behind the birth instant would otherwise run the closed form
    // with a negative exponent and fling the particle backwards along its
    // trajectory — which is exactly what an emitter that pre-rolls does to
    // every particle it has not yet reached.
    out.elapsed = in.systemTime - in.birthTime;
    if (out.elapsed < 0.0f)
        out.elapsed = 0.0f;

    // The shader recovers the mass by reciprocal — the element carries only
    // the inverse — and NEGATES the gravity lane on the way in.
    const f32 mass = 1.0f / in.invMass;
    const auto r = vs::CalculateDisplacementAndVelocity(
        out.elapsed, in.velocity0, mass, in.invMass, in.drag, in.invDrag,
        -in.gravityZ);

    out.displacement = r.displacement;
    out.velocity = r.velocity;
    out.position = vs::Add(in.position, r.displacement);
    out.tailLength = in.tailLength;

    const auto type = static_cast<InstanceType>(in.instanceType);
    if (type == InstanceType::FaceTravelDir) {
        // The whole velocity goes into `vInterpolator2`, whose x lane held the
        // tail length for the stretched types — the two uses share a register.
        out.tailLength = r.velocity.x;
    } else if (type == InstanceType::Tail ||
               type == InstanceType::Pinned ||
               type == InstanceType::Trail) {
        if (in.clampedTailLength) {
            // `b_fixedTailLength` means something DIFFERENT here than it does
            // in the frame build: there it picks `max(tail, tail·speed)`, here
            // it picks the clamp's budget, and only the unfixed spelling takes
            // the max. Same flag, two readings, both measured (OP12).
            const f32 speed = vs::Length3(r.velocity);
            const f32 budget =
                in.fixedTailLength
                    ? (in.tailLength * speed) * in.size
                    : (std::max)(in.tailLength, in.tailLength * speed) * in.size;
            const f32 dist = vs::Length3(r.displacement);
            if (dist < budget) {
                out.tailLength =
                    (std::min)(dist / (speed * in.size), in.tailLength);
            }
        }
    }
    return out;
}

void ElementList::Reset(usize count) {
    const i32 n = static_cast<i32>(count);
    next.assign(count, kSentinel);
    prev.assign(count, kNull);
    for (i32 i = 0; i < n; ++i) {
        next[static_cast<usize>(i)] = (i + 1 < n) ? i + 1 : kSentinel;
        prev[static_cast<usize>(i)] = (i > 0) ? i - 1 : kNull;
    }
    head = n > 0 ? 0 : kSentinel;
    tail = n > 0 ? n - 1 : kNull;
    freeHead = kNull;
    freeTail = kNull;
    poolCount = static_cast<u32>(count);
}

namespace {

void WalkFrom(const std::vector<i32>& links, i32 start, std::vector<i32>& out) {
    out.clear();
    i32 node = start;
    while (node >= 0 && out.size() < links.size()) {
        out.push_back(node);
        node = links[static_cast<usize>(node)];
    }
}

} // namespace

void ElementList::Walk(std::vector<i32>& out) const {
    WalkFrom(next, head, out);
}

void ElementList::WalkBackward(std::vector<i32>& out) const {
    WalkFrom(prev, tail, out);
}

void ElementList::WalkFree(std::vector<i32>& out) const {
    WalkFrom(next, freeHead, out);
}

void ElementList::Unlink(i32 node) {
    const usize n = static_cast<usize>(node);
    const i32 nxt = next[n];
    if (head == node)
        head = nxt;
    const i32 pv = prev[n];
    if (pv >= 0)
        next[static_cast<usize>(pv)] = nxt;
    // `node->next->listPrev = prev`, and for the last node `node->next` is the
    // sentinel — whose listPrev is the tail. Writing it through the same
    // expression is what keeps the backward walk right when the tail moves.
    if (nxt == kSentinel)
        tail = pv;
    else
        prev[static_cast<usize>(nxt)] = pv;
    prev[n] = kNull;
    next[n] = kNull;

    if (freeTail >= 0)
        next[static_cast<usize>(freeTail)] = node;
    else
        freeHead = node;
    freeTail = node;
    --poolCount;
}

u32 RetireExpired(ElementList& list, std::span<SpawnedElement> elements,
                     f32 emitterTime, RecycleArray& recycle) {
    u32 retired = 0;
    i32 node = list.head;
    while (node >= 0) {
        // Read the successor BEFORE unlinking: the unlink clears both links on
        // the node it frees, so a walk that re-read them would stop dead on
        // the first death.
        const i32 nxt = list.next[static_cast<usize>(node)];
        SpawnedElement& e = elements[static_cast<usize>(node)];
        if (e.deathTime <= emitterTime) {
            if (recycle.enabled && e.vbSlot != -1) {
                recycle.slots.push_back(e.vbSlot);
                e.vbSlot = -1;
            }
            list.Unlink(node);
            ++retired;
        }
        node = nxt;
    }
    return retired;
}

namespace {

/// The gravity multiply `SimulateParticles` gates on. WhiteoutLib names the bit
/// `MultiplyGravityByMass`; the runtime multiplies by the SCENE's scale.
constexpr ParticleFlag kSceneGravity = ParticleFlag::MultiplyGravityByMass;
/// The bounds pass's bit.
constexpr ParticleFlag kParBounds = ParBit::CpuBounds;

/// The swept sphere's radius, which is also the terrain push-out.
constexpr f32 kCollideRadius = renderer::sc2::kParticleCollideRadius;
/// At or below this determinant the emitter matrix counts as singular and a
/// contact comes back through identity — which a MIRRORED emitter, whose
/// determinant is negative, reaches as well.
using renderer::sc2::kMinInvertibleDet;
/// The tangential term needs `|v + wind|²` above this.
using renderer::sc2::kFrictionSpeedSq;
/// A landed particle rests below `|v|² < 3·dt`.
using renderer::sc2::kRestPerDt;

/// Every normalise in the CPU step takes this step after its reciprocal root,
/// and `Update` refines its row lengths through the LENGTH spelling.
using renderer::sc2::NewtonLength;
using renderer::sc2::NewtonRsqrt;


using Mat16 = std::array<f32, 16>;

/// `Mat4Adjugate` (`0x10056A760`), term for term: the transposed cofactors of
/// a row-major 4×4, before any division.
Mat16 Adjugate(const Mat16& a) {
    const f32 v2 = a[9], v65 = a[13], v3 = a[14], v4 = a[1], v5 = a[2], v50 = a[12];
    const f32 v60 = v50 * v4;
    const f32 v45 = a[10];
    const f32 v55 = v4 * v45 - v5 * v2;
    const f32 v49 = v5 * v65 - v4 * v3;
    const f32 v53 = a[8];
    const f32 v6 = v53 * v4;
    const f32 v7 = a[4];
    const f32 v8 = v7 * v4;
    const f32 v9 = a[6];
    const f32 v10 = a[5];
    const f32 v11 = v4 * v9 - v5 * v10;
    const f32 v58 = v7 * v45 - v53 * v9;
    const f32 v67 = v50 * v9 - v7 * v3;
    const f32 v57 = v50 * v5 - a[0] * v3;
    const f32 v12 = a[0] * v9 - v7 * v5;
    const f32 v64 = v7 * v2 - v53 * v10;
    const f32 v52 = v53 * v65;
    const f32 v13 = v7 * v65;
    const f32 v51 = v50 * v2;
    const f32 v48 = v2 * a[0];
    const f32 v14 = a[0] * v10 - v8;
    const f32 v41 = v65 * v9;
    const f32 v42 = v65 * v45;
    const f32 v15 = v3 * v10;
    const f32 v16 = a[11];
    const f32 v17 = (v65 * v9 - v3 * v10) * v16;
    const f32 v18 = a[15];
    const f32 v62 = v53 * v3;
    const f32 v40 = a[0] * v45;
    const f32 v54 = v53 * v5;
    const f32 v61 = v60 - v65 * a[0];
    const f32 v43 = (v50 * v10 - v13) * v45;
    const f32 v66 = (v48 - v6) * v3;
    const f32 v19 = v45 * v10 - v2 * v9;
    const f32 v20 = v3 * v2 - v42;
    const f32 v21 = a[7];
    const f32 v22 = (v20 * v21 + v18 * v19) + v17;
    const f32 v23 = a[3];
    const f32 v24 = (v20 * v23 + v55 * v18) + v49 * v16;
    const f32 v25 = ((v15 - v41) * v23 + v11 * v18) + v49 * v21;
    const f32 v56 = v55 * v21 - (v19 * v23 + v11 * v16);
    const f32 v26 = v62 - v50 * v45;
    const f32 v27 = (v26 * v21 + v58 * v18) + v67 * v16;
    const f32 v63 = (v26 * v23 + (v40 - v54) * v18) + v57 * v16;
    const f32 v68 = (v67 * v23 - v12 * v18) - v57 * v21;
    const f32 v59 = (v58 * v23 + v12 * v16) + (v54 - v40) * v21;
    const f32 v28 = ((v52 - v51) * v21 + v64 * v18) + (v50 * v10 - v13) * v16;
    const f32 v29 = ((v52 - v51) * v23 + (v48 - v6) * v18) + v61 * v16;
    const f32 v30 = v13 - v50 * v10;
    const f32 v31 = (v30 * v23 + v18 * v14) + v61 * v21;
    const f32 v32 = v6 - v48;
    return {v22,
            -v24,
            v25,
            v56,
            -v27,
            v63,
            v68,
            v59,
            v28,
            -v29,
            v31,
            -((v23 * v64 + v16 * v14) + v21 * v32),
            -(((v52 - v51) * v9 + v64 * v3) + v43),
            ((v52 - v51) * v5 + v66) + v61 * v45,
            -((v30 * v5 + v3 * v14) + v61 * v9),
            (v64 * v5 + v14 * v45) + v32 * v9};
}

/// The determinant `SimulateParticles` builds inline before it divides, in its
/// own SSE grouping — the same number the adjugate would give, spelled
/// differently in the last bits.
f32 InlineDeterminant(const Mat16& w) {
    const f32 c0 = (w[7] * (w[8] * w[14] - w[10] * w[12]) + w[15] * (w[4] * w[10] - w[6] * w[8])) +
                   w[11] * (w[12] * w[6] - w[14] * w[4]);
    const f32 c1 = (w[7] * (w[9] * w[14] - w[10] * w[13]) + w[15] * (w[5] * w[10] - w[6] * w[9])) +
                   w[11] * (w[13] * w[6] - w[14] * w[5]);
    const f32 a = w[4] * w[9] - w[8] * w[5];
    const f32 b = w[8] * w[13] - w[9] * w[12];
    const f32 c = w[12] * w[5] - w[4] * w[13];
    const f32 s0 = (b * w[6] + a * w[14]) + c * w[10];
    const f32 s1 = (b * w[7] + a * w[15]) + c * w[11];
    return (w[2] * s1 + w[0] * c1) - (w[3] * s0 + w[1] * c0);
}

/// The way a contact comes back into a local-space emitter: the adjugate over
/// the determinant, or identity with the translation negated below the floor.
struct LocalFrame {
    std::array<f32, 9> r{};
    std::array<f32, 3> t{};
};

LocalFrame BuildLocalFrame(const Mat16& w) {
    LocalFrame f;
    const f32 det = InlineDeterminant(w);
    if (det <= kMinInvertibleDet) {
        f.r = {1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f};
        f.t = {-w[12], -w[13], -w[14]};
        return f;
    }
    const Mat16 adj = Adjugate(w);
    const f32 s = 1.0f / det;
    f.r = {adj[0] * s, adj[1] * s, adj[2] * s, adj[4] * s, adj[5] * s,
           adj[6] * s, adj[8] * s, adj[9] * s, adj[10] * s};
    f.t = {adj[12] * s, adj[13] * s, s * adj[14]};
    return f;
}

Vector3f PointToLocal(const LocalFrame& f, const Vector3f& p) {
    return {(p.z * f.r[6] + p.y * f.r[3]) + (p.x * f.r[0] + f.t[0]),
            (p.z * f.r[7] + p.y * f.r[4]) + (p.x * f.r[1] + f.t[1]),
            (p.z * f.r[8] + p.y * f.r[5]) + (p.x * f.r[2] + f.t[2])};
}

/// Through the inverse basis, and normalised on the way — so the response a
/// local emitter computes is against a unit normal even when it is scaled.
Vector3f NormalToLocal(const LocalFrame& f, const Vector3f& n) {
    const f32 x = n.z * f.r[6] + (n.y * f.r[3] + n.x * f.r[0]);
    const f32 y = n.z * f.r[7] + (n.y * f.r[4] + n.x * f.r[1]);
    const f32 z = n.z * f.r[8] + (n.y * f.r[5] + n.x * f.r[2]);
    const f32 sq = (z * z + x * x) + y * y;
    const f32 r = NewtonRsqrt(sq, 1.0f / std::sqrt(sq));
    return {x * r, y * r, r * z};
}


/// Below `|v|² < 3·dt` a landed particle stops spinning AT THE ANGLE IT
/// LANDED ON: all three rotation keys are overwritten with the curve's value
/// now, truncated back into the s16 they are stored as — not reset to key 0.
void RekeyRotationAtRest(SpawnedElement& e, const SimulateInputs& in) {
    namespace vs = whiteout::flakes::renderer::sc2::vs;
    const f32 age = vs::Saturate((in.emitterTime - e.birthTime) / (e.deathTime - e.birthTime));
    // The reciprocal before the curve, as `EvalAnimCurve1D` builds it (§16.4).
    const f32 invMid = 1.0f / in.rotationMidTime;
    const auto key = [&](usize k) {
        return static_cast<f32>(static_cast<i16>(e.rotation[k])) * (1.0f / 32.0f);
    };
    const f32 angle = vs::InterpolateValue(age, key(0), key(1), key(2), in.rotationMidTime,
                                           invMid, in.rotationMidHold, in.rotationSmoothing);
    const u16 q = static_cast<u16>(static_cast<i32>(angle * 32.0f));
    e.rotation = {q, q, q};
}

/// One element's integration and the type-6 freeze.
///
/// Gravity (unless resting), drag in two branches — linear below `k·dt < 1`,
/// and above it the velocity simply becomes `gravity·dt` — then `v += a·dt` and
/// `p += (v + wind)·dt`. Wind reaches the POSITION only, so it never
/// accumulates into the particle's own momentum.
void StepEuler(SpawnedElement& e, const SimulateInputs& in) {
    Vector3f a{0.0f, 0.0f, 0.0f};
    if ((e.flags & renderer::sc2::kElemAtRest) == 0) {
        a = in.gravity;
        if (Has(in.parFlags, kSceneGravity))
            a = {a.x * in.gravityScale, a.y * in.gravityScale, a.z * in.gravityScale};
    }

    // Over-damped, the drag cancels the whole velocity in one step and what is
    // left is gravity's. Both branches are the binary's; OP9 arms each side of
    // `k·dt == 1` and a zero inverse mass that switches drag off entirely.
    const f32 k = in.drag * e.invMass;
    if (k * in.dt >= 1.0f) {
        a = {a.x - e.velocity.x / in.dt, a.y - e.velocity.y / in.dt,
             a.z - e.velocity.z / in.dt};
    } else {
        a = {a.x - k * e.velocity.x, a.y - k * e.velocity.y, a.z - k * e.velocity.z};
    }

    const Vector3f before = e.velocity;
    e.velocity = {e.velocity.x + a.x * in.dt, e.velocity.y + a.y * in.dt,
                  e.velocity.z + a.z * in.dt};
    const Vector3f wind{in.wind.x * in.windMultiplier, in.wind.y * in.windMultiplier,
                        in.wind.z * in.windMultiplier};
    e.position = {e.position.x + (e.velocity.x + wind.x) * in.dt,
                  e.position.y + (e.velocity.y + wind.y) * in.dt,
                  e.position.z + (e.velocity.z + wind.z) * in.dt};

    // The threshold reads the stepped velocity and the direction the one it
    // stepped FROM (RE §6). No OP9 vector separates the two — every freeze row
    // runs without gravity or drag — so this follows the disassembly.
    if (in.instanceType == static_cast<u32>(InstanceType::TerrainDirOriented) &&
        (e.flags & renderer::sc2::kElemOrientationFrozen) == 0) {
        const Vector3f& v = e.velocity;
        if ((v.z * v.z + v.x * v.x) + v.y * v.y < kFreezeSpeedSq) {
            const f32 lsq = (before.z * before.z + before.x * before.x) + before.y * before.y;
            // An exact reciprocal root and one Newton step. A zero velocity
            // turns the refinement into a NaN, and that is what sends it to
            // (1,0,0) — the binary classifies the result, not the length.
            const f32 r = NewtonRsqrt(lsq, 1.0f / std::sqrt(lsq));
            if (std::isfinite(r))
                e.orientVec = {before.x * r, before.y * r, before.z * r};
            else
                e.orientVec = {1.0f, 0.0f, 0.0f};
            e.flags = static_cast<u16>(e.flags | renderer::sc2::kElemOrientationFrozen);
        }
    }
}

} // namespace

namespace {

/// The collision half of one element's sub-step: the swept query in world
/// space, the contact back into the element's space, the response and its
/// rest transition, the bounce count, and what a hit asks of child 0.
void CollideElement(SpawnedElement& e, const Vector3f& from, const SimulateInputs& in,
                    const Collider& collider, const LocalFrame& local, const Vector3f& wind,
                    renderer::sc2::Rng& rng, ChildRequests& children) {
    const Vector3f a = in.worldSpace ? from : renderer::sc2::vs::MulPointMat4(from, in.worldMatrix);
    const Vector3f b =
        in.worldSpace ? e.position : renderer::sc2::vs::MulPointMat4(e.position, in.worldMatrix);

    Contact c;
    bool terrainHit = false;
    if ((e.flags & renderer::sc2::kElemCollideTerrain) != 0 && collider.terrain) {
        Contact t;
        if (collider.terrain(collider.ctx, a, b, t) && t.hit) {
            // Pushed out in WORLD space, and the time overwritten with 1 — so
            // a terrain bounce always lands exactly on the pushed point.
            t.position = {t.normal.x * kCollideRadius + t.position.x,
                          t.normal.y * kCollideRadius + t.position.y,
                          t.normal.z * kCollideRadius + t.position.z};
            t.toi = 1.0f;
            c = t;
            terrainHit = true;
        }
    }
    bool objectHit = false;
    if ((e.flags & renderer::sc2::kElemCollideObjects) != 0 && collider.objects) {
        Contact o;
        if (collider.objects(collider.ctx, a, b, o) && o.hit) {
            c = o;
            objectHit = true;
        }
    }
    if (!terrainHit && !objectHit)
        return;
    // Retail swaps in the terrain's own normal field here (`TestShapeCutout`,
    // then `SampleVectorField2D`). A viewer's ground answers with its normal
    // already and has no cutout to test.

    Vector3f p = c.position;
    Vector3f n = c.normal;
    if (!in.worldSpace) {
        p = PointToLocal(local, p);
        n = NormalToLocal(local, n);
    }

    // A particle already leaving the surface is not bounced, does not land on
    // the contact, and asks nothing of its child.
    const Vector3f mv{wind.x + e.velocity.x, wind.y + e.velocity.y, wind.z + e.velocity.z};
    const f32 d = (n.z * mv.z + n.x * mv.x) + n.y * mv.y;
    if (d >= 0.0f)
        return;

    const Vector3f vn{d * n.x, d * n.y, n.z * d};
    const f32 nb = -in.bounce;
    Vector3f r{nb * vn.x, nb * vn.y, nb * vn.z};
    // `|v + wind|²` and `v + wind − vn`: the friction gate and the tangent both
    // read the MOVING velocity, not the particle's own.
    if ((mv.z * mv.z + mv.x * mv.x) + mv.y * mv.y > kFrictionSpeedSq) {
        r.x = r.x + in.friction * (mv.x - vn.x);
        r.y = r.y + in.friction * (mv.y - vn.y);
        r.z = r.z + (mv.z - vn.z) * in.friction;
    }

    // The wind comes back OUT, so it never accumulates into the particle's
    // own momentum across a bounce either.
    e.velocity = {r.x - wind.x, r.y - wind.y, r.z - wind.z};
    const Vector3f& v = e.velocity;
    if ((v.z * v.z + v.x * v.x) + v.y * v.y < kRestPerDt * in.dt) {
        e.velocity = {0.0f, 0.0f, 0.0f};
        e.flags = static_cast<u16>(e.flags | renderer::sc2::kElemAtRest);
        if (in.instanceType != static_cast<u32>(InstanceType::TerrainDirOriented))
            RekeyRotationAtRest(e, in);
    }

    // The rest of the step from the contact. Only an object contact leaves
    // `toi` below 1, and it continues with the response as it was BEFORE the
    // wind came out and before a rest zeroed it.
    const f32 rest = 1.0f - c.toi;
    e.position = {(r.x * in.dt) * rest + p.x, (r.y * in.dt) * rest + p.y,
                  rest * (r.z * in.dt) + p.z};
    if (++e.bounceCount == static_cast<i32>(in.collisionDieBounce))
        e.deathTime = 0.0f;

    // Element flag 2 skips the chance roll, so the two arms leave the stream
    // at different words and the count drawn next differs (OP9 `cdraw`).
    const bool forced = (e.flags & renderer::sc2::kElemForced) != 0;
    // Retail draws both chances inline off `RandomNextU32` rather than through
    // `Rand_RangeF` (RE §16.10); `RangeF(0, 1)` is the same bits, its span
    // exactly 1 and its base exactly 0.
    if (in.collisionChild && (forced || rng.RangeF(0.0f, 1.0f) <= in.collisionSpawnChance)) {
        SpawnRequest req;
        req.position = p;
        req.velocityScale = {in.collisionSpawnEnergy, in.collisionSpawnEnergy,
                             in.collisionSpawnEnergy};
        req.orientVec = n;
        if (!in.worldSpace) {
            // Out through the matrix, and the normal NOT renormalised: a scaled
            // emitter hands its child a scaled normal (OP9 `cspawn`). The
            // binary's sums commute to vs's grouping bit for bit.
            req.position = renderer::sc2::vs::MulPointMat4(p, in.worldMatrix);
            req.orientVec = renderer::sc2::vs::MulVecMat4As3(n, in.worldMatrix);
        }
        for (u32 k = rng.RangeInt(in.collisionSpawnMin, in.collisionSpawnMax); k != 0; --k)
            children.collision.push_back(req);
        e.deathTime = 0.0f;
    }
    if (in.splat && (forced || rng.RangeF(0.0f, 1.0f) <= in.splatChance))
        e.deathTime = 0.0f;
}

/// `trailAccum` drains on a STRICT `> 1`, one request per whole unit, each with
/// a fresh random direction as its velocity scale (RE §16.10).
void QueueTrails(SpawnedElement& e, const SimulateInputs& in, renderer::sc2::Rng& rng,
                 ChildRequests& children) {
    SpawnRequest req;
    req.position = e.position;
    req.velocityScale = {0.0f, 0.0f, 0.0f};
    req.orientVec = {0.0f, 0.0f, 0.0f};
    e.trailAccum = in.trailRate * in.dt + e.trailAccum;
    if (e.trailAccum <= 1.0f)
        return;
    do {
        const f32 x = rng.RangeF(-1.0f, 1.0f);
        const f32 y = rng.RangeF(-1.0f, 1.0f);
        const f32 z = rng.RangeF(-1.0f, 1.0f);
        const f32 sq = (y * y + x * x) + z * z;
        const f32 r = NewtonRsqrt(sq, 1.0f / std::sqrt(sq));
        req.velocityScale = {x * r, y * r, r * z};
        children.trail.push_back(req);
        e.trailAccum = e.trailAccum + -1.0f;
    } while (e.trailAccum > 1.0f);
}

} // namespace

SimulateResult SimulateParticles(ElementList& list,
                                       std::span<SpawnedElement> elements,
                                       const SimulateInputs& in,
                                       const Collider& collider, renderer::sc2::Rng& rng,
                                       ChildRequests& children, std::vector<i32>* killed) {
    SimulateResult out;
    const Vector3f wind{in.wind.x * in.windMultiplier, in.wind.y * in.windMultiplier,
                        in.wind.z * in.windMultiplier};
    const bool bounds = Has(in.parFlags, kParBounds);
    // Built once before the walk, whether or not anything collides, as retail
    // builds it.
    const LocalFrame local = BuildLocalFrame(in.worldMatrix);

    i32 node = list.head;
    while (node >= 0) {
        const i32 nxt = list.next[static_cast<usize>(node)];
        SpawnedElement& e = elements[static_cast<usize>(node)];
        const Vector3f from = e.position;
        StepEuler(e, in);

        // Only a particle still alive is collided, so one that dies this
        // sub-step neither bounces nor asks its child for a spawn.
        if (e.deathTime > in.emitterTime &&
            (e.flags & (renderer::sc2::kElemCollideTerrain | renderer::sc2::kElemCollideObjects)) != 0 &&
            in.collisionEnabled) {
            CollideElement(e, from, in, collider, local, wind, rng, children);
        }

        // After the collision, so a particle a hit killed lays no trail.
        if (e.deathTime > in.emitterTime && (e.flags & renderer::sc2::kElemTrail) != 0 && in.trailChild)
            QueueTrails(e, in, rng, children);

        if (bounds) {
            out.boundsMin = {(std::min)(out.boundsMin.x, e.position.x),
                             (std::min)(out.boundsMin.y, e.position.y),
                             (std::min)(out.boundsMin.z, e.position.z)};
            out.boundsMax = {(std::max)(out.boundsMax.x, e.position.x),
                             (std::max)(out.boundsMax.y, e.position.y),
                             (std::max)(out.boundsMax.z, e.position.z)};
        }

        // Both kill tests run after the bounds pass, so a particle dying this
        // step still widens them. The radius is compared AS AUTHORED against the
        // squared distance (OP9's radius-10 row). See SC2_PARTICLE_RE.md §17.7.
        const f32 dx = e.position.x - in.origin.x;
        const f32 dy = e.position.y - in.origin.y;
        const f32 dz = e.position.z - in.origin.z;
        const bool survives =
            e.deathTime > in.emitterTime &&
            (in.killRadius <= 0.0f || dz * dz + (dy * dy + dx * dx) <= in.killRadius);
        if (!survives) {
            list.Unlink(node);
            ++out.killed;
            if (killed != nullptr)
                killed->push_back(node);
        }
        node = nxt;
    }
    return out;
}

/// Retail's cap on one emitter's pending requests (`CParticleSystem+0x358`).
constexpr usize kMaxSpawnRequests = 128;

void QueueSpawnRequest(std::vector<SpawnRequest>& queue, const SpawnRequest& req) {
    if (queue.size() < kMaxSpawnRequests)
        queue.push_back(req);
}

Vector3f ChildScale(const std::array<f32, 16>& w) {
    const f32 sq0 = w[2] * w[2] + (w[1] * w[1] + w[0] * w[0]);
    const f32 sq1 = w[6] * w[6] + (w[5] * w[5] + w[4] * w[4]);
    const f32 sq2 = w[10] * w[10] + (w[9] * w[9] + w[8] * w[8]);
#if WDX_SC2_HAS_SSE
    // Rows 0 and 1 in one packed `rsqrtps`, whose seed is the hardware's
    // approximation - which is why the gate holds these two lanes to the rsqrt
    // bound and not to the bit.
    const __m128 seeds = _mm_rsqrt_ps(_mm_set_ps(0.0f, 0.0f, sq1, sq0));
    const f32 seed0 = _mm_cvtss_f32(seeds);
    const f32 seed1 = _mm_cvtss_f32(_mm_shuffle_ps(seeds, seeds, _MM_SHUFFLE(1, 1, 1, 1)));
#else
    const f32 seed0 = 1.0f / std::sqrt(sq0);
    const f32 seed1 = 1.0f / std::sqrt(sq1);
#endif
    // Each lane masked against a length-squared of exactly zero, so a collapsed
    // row pushes 0 where the refinement would have made a NaN.
    return {sq0 != 0.0f ? NewtonLength(sq0, seed0) : 0.0f,
            sq1 != 0.0f ? NewtonLength(sq1, seed1) : 0.0f,
            sq2 != 0.0f ? NewtonLength(sq2, 1.0f / std::sqrt(sq2)) : 0.0f};
}

Matrix44f PushChildScale(const Matrix44f& childBone, const Vector3f& localScale,
                            const Vector3f& pushed) {
    Matrix44f out = childBone;
    const f32 local[3] = {localScale.x, localScale.y, localScale.z};
    const f32 push[3] = {pushed.x, pushed.y, pushed.z};
    for (usize r = 0; r < 3; ++r) {
        if (local[r] == 0.0f)
            continue;
        const f32 k = push[r] / local[r];
        for (usize c = 0; c < 3; ++c)
            out.data[r][c] = childBone.data[r][c] * k;
    }
    return out;
}

} // namespace whiteout::flakes::renderer::particle::sc2
