#include "renderer/particle/particle_geometry.h"
#include "renderer/particle/particle_constants.h"
#include "whiteout/flakes/util/coordinate_system.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace whiteout::flakes::renderer::particle {

namespace {

constexpr f32 kEpsilon = 1e-6f;

constexpr f32 vc[4][2] = {
    {-1.0f, 1.0f},
    {-1.0f, -1.0f},
    {1.0f, 1.0f},
    {1.0f, -1.0f},
};

struct CameraBasis {
    Vector3f fwd;
    Vector3f right;
    Vector3f up;
};

CameraBasis BasisFromView(const Matrix44f& viewMatrix) {
    CameraBasis b{};

    b.right = {viewMatrix.data[0][0], viewMatrix.data[1][0], viewMatrix.data[2][0]};
    b.up = {viewMatrix.data[0][1], viewMatrix.data[1][1], viewMatrix.data[2][1]};

    b.fwd = {-viewMatrix.data[0][2], -viewMatrix.data[1][2], -viewMatrix.data[2][2]};
    return b;
}

inline Vector3f Cross(const Vector3f& a, const Vector3f& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

inline f32 Dot(const Vector3f& a, const Vector3f& b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

inline f32 LengthSq(const Vector3f& v) {
    return v.x * v.x + v.y * v.y + v.z * v.z;
}

inline Vector3f Normalize(const Vector3f& v, const Vector3f& fallback = {0, 0, 1}) {
    f32 l2 = LengthSq(v);
    if (l2 < kEpsilon)
        return fallback;
    f32 inv = 1.0f / std::sqrt(l2);
    return {v.x * inv, v.y * inv, v.z * inv};
}

/// One sprite-sheet cell's texture rectangle.
struct CellRect {
    f32 u0, v0, u1, v1;
};

CellRect CellRectFor(const SpriteSheet& sheet, i32 cell) {
    u32 cols = sheet.cols;
    if (cols == 0)
        cols = 1;

    u32 col, row;
    if ((cols & (cols - 1)) == 0) {

        col = static_cast<u32>(cell) & (cols - 1);
        row = static_cast<u32>(cell) >> sheet.log2Cols;
    } else {
        col = static_cast<u32>(cell) % cols;
        row = static_cast<u32>(cell) / cols;
    }
    const f32 u = col * sheet.ooWidth;
    const f32 v = row * sheet.ooHeight;
    return {u, v, u + sheet.ooWidth, v + sheet.ooHeight};
}

/// One live particle in draw order, with where it is in the world — resolved
/// once, for the sort key and the quad alike.
struct DrawRecord {
    u32 aliveIndex;
    f32 viewZ;
    Vector3f worldPos;
    Vector3f worldVel;
};

/// The emitter's particles in the order its builder draws them: alive order,
/// or back to front with an index tie-break under @p sortZ.
///
/// @p resolve places one particle in the world. Filled into a per-thread
/// buffer, cleared and never freed, so a steady frame allocates nothing; the
/// reference is good until the next call on this thread.
template <class Resolve>
std::vector<DrawRecord>& DrawOrder(const ParticlePool& pool, bool sortZ, const CameraBasis& cam,
                                   Resolve&& resolve) {
    thread_local std::vector<DrawRecord> order;
    order.clear();
    order.reserve(pool.AliveCount());
    for (usize i = 0; i < pool.AliveCount(); ++i) {
        DrawRecord rec{static_cast<u32>(i), 0.0f, {}, {}};
        resolve(pool[pool.AliveAt(i)], rec.worldPos, rec.worldVel);
        if (sortZ)
            rec.viewZ = Dot(rec.worldPos, cam.fwd);
        order.push_back(rec);
    }
    if (sortZ) {
        // Index tie-break: a burst spawned at one point gives every particle
        // the same viewZ, and equal elements resolve unspecified otherwise.
        std::sort(order.begin(), order.end(), [](const DrawRecord& a, const DrawRecord& b) {
            if (a.viewZ != b.viewZ)
                return a.viewZ > b.viewZ;
            return a.aliveIndex < b.aliveIndex;
        });
    }
    return order;
}

// The one winding every quad this file writes takes: corners c0..c3 as the
// two triangles (c0, c1, c2) and (c3, c2, c1), with the UVs (u0,v0), (u0,v1),
// (u1,v0), (u1,v1) on c0..c3.
void EmitStrip(std::vector<Vertex>& out, const Vector3f& c0, const Vector3f& c1,
               const Vector3f& c2, const Vector3f& c3, const Vector4f& color,
               const Vector3f& normal, f32 u0, f32 v0, f32 u1, f32 v1) {
    const Vector2f uv0{u0, v0}, uv1{u0, v1}, uv2{u1, v0}, uv3{u1, v1};
    out.push_back({c0, normal, color, uv0});
    out.push_back({c1, normal, color, uv1});
    out.push_back({c2, normal, color, uv2});
    out.push_back({c3, normal, color, uv3});
    out.push_back({c2, normal, color, uv2});
    out.push_back({c1, normal, color, uv1});
}

// Corner order, shared by both dialects: -A+B, -A-B, +A+B, +A-B with UVs
// (0,0), (0,1), (1,0), (1,1). WC3 spells it out as the `vc` table above; the
// WoW builder writes the same four combinations here, which is how the two
// clients' quads end up interchangeable at this level.
void EmitQuad(std::vector<Vertex>& out, const Vector3f& centre, const Vector3f& a,
              const Vector3f& b, const Vector4f& color, const Vector3f& normal, f32 u0, f32 v0,
              f32 u1, f32 v1) {
    const Vector3f c0{centre.x - a.x + b.x, centre.y - a.y + b.y, centre.z - a.z + b.z};
    const Vector3f c1{centre.x - a.x - b.x, centre.y - a.y - b.y, centre.z - a.z - b.z};
    const Vector3f c2{centre.x + a.x + b.x, centre.y + a.y + b.y, centre.z + a.z + b.z};
    const Vector3f c3{centre.x + a.x - b.x, centre.y + a.y - b.y, centre.z + a.z - b.z};
    EmitStrip(out, c0, c1, c2, c3, color, normal, u0, v0, u1, v1);
}

// The client's own guards. A velocity shorter than `kVelocityEpsilon` cannot
// orient a quad, and a tail whose on-screen length is under 1/36 of a unit is
// drawn as a plain billboard instead of stretched. The tail one is a length in
// the emitter's own units, so it scales with the model; the velocity one is a
// squared-magnitude floor and does not.
constexpr f32 kTailMinPlaneLength = 1.0f / 36.0f;

i32 BuildWc3Geometry(const Emitter2& emitter, const BuildGeometryInput& in,
                     std::vector<Vertex>& out) {
    const ParticlePool& pool = emitter.Pool();

    const bool hasHead = emitter.Desc().hasHead;
    const bool hasTail = emitter.Desc().hasTail;
    if (!hasHead && !hasTail)
        return 0;

    const bool modelSpace = emitter.Desc().modelSpace;
    const bool xyQuads = emitter.Desc().xyQuads;
    const f32 angVel = emitter.Desc().angularVelocity;
    const bool useAngVel = std::abs(angVel) > kEpsilon;
    const f32 tailLength = emitter.Desc().tailLength;
    const SpriteSheet& sheet = emitter.Desc().sheet;
    const LifetimeCurves& curves = emitter.Desc().curves;
    const f32 lifeSpan = emitter.Desc().lifeSpan;
    const f32 ooLifeSpan = (lifeSpan > 0.0f) ? (1.0f / lifeSpan) : 0.0f;

    CameraBasis cam = BasisFromView(*in.worldToView);

    const CoordSpace emSpace = emitter.Desc().coordSpace;
    const bool needsConvert = (emSpace != CoordinateSystem::Default());

    auto resolveWorld = [&](const Particle2& p, Vector3f& outPos, Vector3f& outVel) {
        Vector3f pos = p.position;
        Vector3f vel = p.velocity;
        if (modelSpace) {
            pos = whiteout::transform_point(pos, emitter.ModelToWorld());
            vel = whiteout::transform_normal(vel, emitter.ModelToWorld());
        }
        if (needsConvert) {
            pos = CoordinateSystem::ToDefault(emSpace, pos);
            vel = CoordinateSystem::ToDefaultDir(emSpace, vel);
        }
        outPos = pos;
        outVel = vel;
    };

    const std::vector<DrawRecord>& order =
        DrawOrder(pool, emitter.Desc().sortZ, cam, resolveWorld);

    const usize startSize = out.size();
    const Vector3f normal = {0.0f, 0.0f, 1.0f};

    for (const DrawRecord& rec : order) {
        u32 idx = pool.AliveAt(rec.aliveIndex);
        const Particle2& p = pool[idx];
        const Vector3f& worldPos = rec.worldPos;
        const Vector3f& worldVel = rec.worldVel;

        // Normalised age drives every lifetime curve; the aux word rides along
        // as a segment hint so the lookup does not have to search.
        const f32 u = ooLifeSpan * p.age;
        const u32 hint = p.Cursor();

        const Vector3f rgb = curves.color.Evaluate(u, hint);
        const f32 alpha = curves.alpha.Evaluate(u, hint);
        const Vector2f size = curves.size.Evaluate(u, hint);
        const i32 headCell = curves.headCells.Evaluate(u, hint);
        const i32 tailCell = curves.tailCells.Evaluate(u, hint);

        Vector4f vcol = ImVector::FromUnitFloat(rgb.x, rgb.y, rgb.z, alpha).ToVec4();

        // WC3 quads are square; the curve carries 2D size for M2/M3, so the
        // head uses x and the tail half-width uses y.
        const f32 corner = size.x;

        if (hasHead) {
            const CellRect cell = CellRectFor(sheet, headCell);

            Vector3f right = cam.right;
            Vector3f up = cam.up;

            if (useAngVel) {

                f32 theta = p.age * angVel;
                f32 ct = std::cos(theta), st = std::sin(theta);
                Vector3f r2 = {right.x * ct + up.x * st, right.y * ct + up.y * st,
                               right.z * ct + up.z * st};
                Vector3f u2 = {up.x * ct - right.x * st, up.y * ct - right.y * st,
                               up.z * ct - right.z * st};
                right = r2;
                up = u2;
            } else if (xyQuads) {

                Vector3f localVelXY = {p.velocity.x, p.velocity.y, 0.0f};
                Vector3f localPerpXY = {p.velocity.y, -p.velocity.x, 0.0f};

                Vector3f worldPvel = localVelXY;
                Vector3f worldPvelPerp = localPerpXY;
                if (modelSpace) {
                    worldPvel = whiteout::transform_normal(localVelXY, emitter.ModelToWorld());
                    worldPvelPerp = whiteout::transform_normal(localPerpXY, emitter.ModelToWorld());
                }
                if (needsConvert) {
                    worldPvel = CoordinateSystem::ToDefaultDir(emSpace, worldPvel);
                    worldPvelPerp = CoordinateSystem::ToDefaultDir(emSpace, worldPvelPerp);
                }

                f32 mag2 = LengthSq(worldPvel);
                if (mag2 > kEpsilon) {

                    right = Normalize(worldPvelPerp);
                    up = Normalize(worldPvel);
                }
            }

            Vector3f corners[4];
            for (i32 c = 0; c < 4; ++c) {
                f32 sx = vc[c][0] * corner;
                f32 sy = vc[c][1] * corner;
                corners[c] = {worldPos.x + right.x * sx + up.x * sy,
                              worldPos.y + right.y * sx + up.y * sy,
                              worldPos.z + right.z * sx + up.z * sy};
            }
            EmitStrip(out, corners[0], corners[1], corners[2], corners[3], vcol, normal, cell.u0,
                      cell.v0, cell.u1, cell.v1);
        }

        if (hasTail) {
            const CellRect cell = CellRectFor(sheet, tailCell);

            Vector3f negVel = {-worldVel.x * tailLength, -worldVel.y * tailLength,
                               -worldVel.z * tailLength};
            if (LengthSq(negVel) < kEpsilon)
                continue;

            Vector3f tailEnd = {worldPos.x + negVel.x, worldPos.y + negVel.y,
                                worldPos.z + negVel.z};

            Vector3f tailDir = Normalize(negVel);
            Vector3f perp = Cross(tailDir, cam.fwd);
            if (LengthSq(perp) < kEpsilon) {

                Vector3f altUp =
                    (std::abs(tailDir.z) > kTailVerticalCos) ? Vector3f{0, 1, 0} : Vector3f{0, 0, 1};
                perp = Cross(tailDir, altUp);
            }
            perp = Normalize(perp);

            Vector3f w = {perp.x * corner, perp.y * corner, perp.z * corner};

            Vector3f hl = {worldPos.x - w.x, worldPos.y - w.y, worldPos.z - w.z};
            Vector3f hr = {worldPos.x + w.x, worldPos.y + w.y, worldPos.z + w.z};
            Vector3f tl = {tailEnd.x - w.x, tailEnd.y - w.y, tailEnd.z - w.z};
            Vector3f tr = {tailEnd.x + w.x, tailEnd.y + w.y, tailEnd.z + w.z};

            EmitStrip(out, hl, hr, tl, tr, vcol, normal, cell.u0, cell.v0, cell.u1, cell.v1);
        }
    }

    return static_cast<i32>(out.size() - startSize);
}

// ===========================================================================
// WoW builder.
//
// A separate function rather than branches inside the WC3 one, for the same
// reason the two integrators are separate: the term order can only be right for
// one client, and every WC3 vertex is pinned bit-for-bit by the L2 baselines.
// Nothing below is reachable from a WC3 emitter.
//
// The client works in view space — it transforms each particle by
// s_particleToView and builds the quad from view-space axes. This builds the
// same quad in world space from the camera basis, which is the identical
// construction under an orthonormal view transform and is what the rest of this
// renderer expects. Reference: IBuildVertices<CParticle2,0,CGxVertexPCT>
// @0x1016af940, InterpolateAllTracks<0> @0x1016a1b70, GetSpin @0x1016a2120.
// ===========================================================================
i32 BuildWowGeometry(const Emitter2& emitter, const BuildGeometryInput& in,
                     std::vector<Vertex>& out) {
    const EmitterDesc& d = emitter.Desc();
    const ParticlePool& pool = emitter.Pool();

    const bool hasHead = d.hasHead;
    const bool hasTail = d.hasTail;
    if (!hasHead && !hasTail)
        return 0;

    const CameraBasis cam = BasisFromView(*in.worldToView);
    const CoordSpace emSpace = d.coordSpace;
    const bool needsConvert = (emSpace != CoordinateSystem::Default());
    const Matrix44f& toWorld = emitter.ModelToWorld();

    auto resolveWorld = [&](const Particle2& p, Vector3f& outPos, Vector3f& outVel) {
        Vector3f pos = p.position;
        Vector3f vel = p.velocity;
        if (d.modelSpace) {
            pos = whiteout::transform_point(pos, toWorld);
            vel = whiteout::transform_normal(vel, toWorld);
        }
        if (needsConvert) {
            pos = CoordinateSystem::ToDefault(emSpace, pos);
            vel = CoordinateSystem::ToDefaultDir(emSpace, vel);
        }
        outPos = pos;
        outVel = vel;
    };

    std::vector<DrawRecord>& order = DrawOrder(pool, d.sortZ, cam, resolveWorld);
    if (d.sortZ && emitter.Behavior().sortedBuilderDefect && order.size() > 1) {
        // The shipped defect. All six sorted `IBuildVertices` clones read
        // `&s_pq.top()` AFTER calling pop(), so the record they draw is the
        // one the heap just sifted into that slot: for N particles the
        // emitted order is ranks 2,3,...,N,N — the farthest is dropped and
        // the nearest drawn twice. Reproduce, do not repair; fixing it would
        // diverge from every depth-sorted emitter in the client.
        for (usize i = 0; i + 1 < order.size(); ++i)
            order[i] = order[i + 1];
    }

    // Renderer units per model unit, and — separately — the emitter bone's own
    // scale, which InheritBoneScale opts into as a SQUARE ROOT (the client's
    // m_scaleFactor, extracted in UpdateXform). The emitter transform carries
    // both, so dividing the first out leaves the second.
    const f32 unit = emitter.UnitScale();
    f32 boneScale = 1.0f;
    if (d.inheritBoneScale) {
        const Vector3f row{toWorld.data[0][0], toWorld.data[0][1], toWorld.data[0][2]};
        const f32 len = std::sqrt(LengthSq(row));
        boneScale = std::sqrt((unit > 0.0f) ? (len / unit) : len);
    }

    // Refraction and multi-texture only: the second and third UV sets,
    // appended one per vertex alongside `out`.
    // `uvN = particleUV[N] + corner01 * multiTexScale[N]`, and the corner is the
    // SAME 0/1 pair the sprite cell uses — the client scales one `s_renderTC`
    // entry three ways per vertex
    // (IBuildVertices<CMultiTexParticle,0,CGxVertexPCT3> @0x1016c5865).
    const bool extraLayers = d.UsesMultiTexLayers() && in.extraUV != nullptr;
    const std::vector<MultiTexState>& mtx = emitter.MultiTex();
    u32 particleIndex = 0;
    auto appendExtraUV = [&](usize from, f32 u0, f32 v0) {
        if (!extraLayers || particleIndex >= mtx.size())
            return;
        const MultiTexState& m = mtx[particleIndex];
        for (usize k = from; k < out.size(); ++k) {
            // Exact, not a division: every quad UV this builder writes is
            // literally one of the two cell corners it was handed.
            const f32 su = (out[k].uv.x == u0) ? 0.0f : 1.0f;
            const f32 sv = (out[k].uv.y == v0) ? 0.0f : 1.0f;
            in.extraUV->push_back({m.uv[0].x + su * d.multiTexScale[0],
                                   m.uv[0].y + sv * d.multiTexScale[0],
                                   m.uv[1].x + su * d.multiTexScale[1],
                                   m.uv[1].y + sv * d.multiTexScale[1]});
        }
    };

    const f32* twinkle = TwinkleTable();
    const bool twinkles = (d.twinklePercent < 1.0f) || (d.twinkleVary != 0.0f);
    const bool scaleAlpha = emitter.Behavior().modelAlphaScalesParticles;
    // The client decides whether to rotate at all from the two SPEED terms
    // alone, so an emitter with a base spin and no spin speed draws unrotated.
    const bool spins = (d.spinSpeed != 0.0f) || (d.spinSpeedVariation != 0.0f);

    const u32 cells = d.sheet.rows * d.sheet.cols;
    const u32 cellCount = (cells > 0) ? cells : 1u;
    const u32 cellMask = cellCount - 1;
    const f32 tailMin = kTailMinPlaneLength * unit;
    const f32 tailMinSq = tailMin * tailMin;

    const usize startSize = out.size();
    const Vector3f normal{0.0f, 0.0f, 1.0f};

    for (const DrawRecord& rec : order) {
        particleIndex = pool.AliveAt(rec.aliveIndex);
        const Particle2& p = pool[particleIndex];
        const u16 seed = p.RenderSeed();

        // Twinkle culls before anything else is sampled: a blinked-off particle
        // costs only its place in the queue.
        u32 twIdx = 0;
        if (twinkles)
            twIdx = TwinkleIndex(seed, p.age, d.twinkleSpeed);
        const f32 twRand = twinkle[twIdx];
        if (d.twinklePercent < twRand)
            continue;

        // Per-particle life fraction: WoW gives every particle its own lifespan,
        // so this is not the emitter's normalised age.
        const f32 life = emitter.EffectiveLifeSpan(p);
        const f32 t = (life > 0.0f) ? (p.age / life) : 0.0f;

        // Hint 0 throughout. WoW tracks are one to three keys in practice and
        // the aux word is spoken for (variance + seed), so there is no cursor to
        // carry — `FindSegment` gives the same answer either way.
        const Vector3f rgb = d.curves.color.Evaluate(t, 0);
        f32 alpha = d.curves.alpha.Evaluate(t, 0);
        if (scaleAlpha)
            alpha *= emitter.ModelAlpha();
        Vector2f size = d.curves.size.Evaluate(t, 0);

        // Render randoms, in the client's order: the optional cell draw comes
        // FIRST, so turning ChooseRandomTexture on shifts the size jitter with
        // it. Everything here is a pure function of (seed, age), which is what
        // makes drawing the same particle twice in one frame identical.
        RndSeed rs(seed);
        i32 headCell = 0;
        i32 tailCell = 0;
        if (!d.curves.headCells.Empty()) {
            const u32 v = static_cast<u32>(d.curves.headCells.Evaluate(t, 0));
            headCell = static_cast<i32>((v + emitter.BaseCell()) & cellMask);
        } else if (d.chooseRandomTexture) {
            headCell = static_cast<i32>(CRandom::dice_(cellCount, rs));
        }
        if (!d.curves.tailCells.Empty()) {
            const u32 v = static_cast<u32>(d.curves.tailCells.Evaluate(t, 0));
            tailCell = static_cast<i32>((v + emitter.BaseCell()) & cellMask);
        }

        const f32 sizeRand = CRandom::reals_(rs);
        if (d.unscaledSizeVariation) {
            const f32 sizeRand2 = CRandom::reals_(rs);
            size.x *= (std::max)(1.0f + sizeRand * d.sizeVariation.x, kSizeJitterFloor);
            size.y *= (std::max)(1.0f + sizeRand2 * d.sizeVariation.y, kSizeJitterFloor);
        } else {
            // One draw, and only the x variation, applied to both axes.
            const f32 m = (std::max)(1.0f + sizeRand * d.sizeVariation.x, kSizeJitterFloor);
            size.x *= m;
            size.y *= m;
        }

        const f32 tw = d.twinkleBase + d.twinkleVary * twRand;
        size.x *= tw;
        size.y *= tw;
        if (d.inheritBoneScale) {
            size.x *= boneScale;
            size.y *= boneScale;
        }
        // Model units to renderer units. The client never needs this — its whole
        // world is yards — but here the tracks are in model units and the quad
        // is built in renderer ones.
        size.x *= unit;
        size.y *= unit;

        // GetSpin re-seeds from the same particle seed into its own stream, and
        // draws only for the variations that are actually non-zero.
        f32 baseSpin = d.baseSpin;
        f32 spinSpeed = d.spinSpeed;
        if (d.baseSpinVariation != 0.0f || d.spinSpeedVariation != 0.0f) {
            RndSeed ss(seed);
            if (d.baseSpinVariation != 0.0f)
                baseSpin += CRandom::reals_(ss) * d.baseSpinVariation;
            if (d.spinSpeedVariation != 0.0f)
                spinSpeed += CRandom::reals_(ss) * d.spinSpeedVariation;
        }
        // The spun angle, which both the flat and the facing quad take, and
        // which a random-sign emitter negates for half its particles.
        const auto spinAngle = [&] {
            f32 angle = p.age * spinSpeed + baseSpin;
            if (d.negateSpinRandom && (seed & 1u) != 0u)
                angle = -angle;
            return angle;
        };

        const Vector3f& worldPos = rec.worldPos;
        const Vector3f& worldVel = rec.worldVel;

        const Vector4f vcol = ImVector::FromUnitFloat(rgb.x, rgb.y, rgb.z, alpha).ToVec4();

        // The plain screen-aligned pair, kept because the degenerate tail falls
        // back to it whatever the head is doing.
        const Vector3f screenA{cam.right.x * size.x, cam.right.y * size.x,
                               cam.right.z * size.x};
        const Vector3f screenB{cam.up.x * size.y, cam.up.y * size.y, cam.up.z * size.y};

        Vector3f centre = worldPos;
        Vector3f axisA = screenA;
        Vector3f axisB = screenB;

        const f32 velLenSq = LengthSq(worldVel);
        if (d.velocityOrient && velLenSq > kVelocityEpsilon) {
            // Lie the quad along the velocity as the camera sees it, and shorten
            // that axis by how much of the velocity points at the viewer — so a
            // particle flying straight at the camera draws round, not stretched.
            const Vector3f nv{-worldVel.x, -worldVel.y, -worldVel.z};
            const f32 vr = Dot(nv, cam.right);
            const f32 vu = Dot(nv, cam.up);
            const f32 planeSq = vr * vr + vu * vu;
            if (planeSq <= kVelocityEpsilon) {
                axisA = {0.0f, 0.0f, 0.0f};
                axisB = {0.0f, 0.0f, 0.0f};
            } else {
                const f32 planeLen = std::sqrt(planeSq);
                const f32 nx = vr / planeLen;
                const f32 ny = vu / planeLen;
                const f32 along = size.x * (planeLen / std::sqrt(velLenSq));
                axisA = {(cam.right.x * nx + cam.up.x * ny) * along,
                         (cam.right.y * nx + cam.up.y * ny) * along,
                         (cam.right.z * nx + cam.up.z * ny) * along};
                axisB = {(cam.up.x * nx - cam.right.x * ny) * size.y,
                         (cam.up.y * nx - cam.right.y * ny) * size.y,
                         (cam.up.z * nx - cam.right.z * ny) * size.y};
            }
        } else if (d.xyQuads) {
            // XYQuad lies the sprite flat in the world XY plane instead of
            // facing the camera. `s_quadToView` is one matrix for the whole
            // frame, shared by every emitter, so it can only be the world basis
            // — and the emitter caches its third row as the spin axis
            // (RenderParticlesPrep @0x1016a2610), i.e. that plane's normal.
            axisA = {size.x, 0.0f, 0.0f};
            axisB = {0.0f, size.y, 0.0f};
            if (spins) {
                const f32 angle = spinAngle();
                const f32 c = std::cos(angle);
                const f32 s = std::sin(angle);
                axisA = {size.x * c, size.x * s, 0.0f};
                axisB = {-size.y * s, size.y * c, 0.0f};
            }
        } else if (spins) {
            const f32 angle = spinAngle();
            const f32 c = std::cos(angle);
            const f32 s = std::sin(angle);
            axisA = {(cam.right.x * c + cam.up.x * s) * size.x,
                     (cam.right.y * c + cam.up.y * s) * size.x,
                     (cam.right.z * c + cam.up.z * s) * size.x};
            axisB = {(cam.up.x * c - cam.right.x * s) * size.y,
                     (cam.up.y * c - cam.right.y * s) * size.y,
                     (cam.up.z * c - cam.right.z * s) * size.y};
            // Push the quad along its own spun up-axis, so spin becomes an orbit
            // around the particle rather than a rotation in place. The tail is
            // drawn from the moved centre too, which is the client's own read of
            // the same variable.
            if (d.offsetHeadBySpin)
                centre = {centre.x + axisB.x, centre.y + axisB.y, centre.z + axisB.z};
        }

        if (hasHead) {
            const CellRect cell = CellRectFor(d.sheet, headCell);
            const usize from = out.size();
            EmitQuad(out, centre, axisA, axisB, vcol, normal, cell.u0, cell.v0, cell.u1, cell.v1);
            appendExtraUV(from, cell.u0, cell.v0);
        }

        if (hasTail) {
            const CellRect cell = CellRectFor(d.sheet, tailCell);

            f32 len = d.tailLength;
            if (d.clampTailToAge && len > p.age)
                len = p.age;
            const Vector3f tail{-worldVel.x * len, -worldVel.y * len, -worldVel.z * len};
            const f32 tr = Dot(tail, cam.right);
            const f32 tu = Dot(tail, cam.up);
            const f32 planeSq = tr * tr + tu * tu;

            const usize tailFrom = out.size();
            if (planeSq >= tailMinSq) {
                const f32 inv = 1.0f / std::sqrt(planeSq);
                // Half-width perpendicular to the tail on screen. Note the axes
                // are scaled separately before the perpendicular is taken, so a
                // non-square particle's tail is not a strict rotation of it —
                // the client's asymmetry, kept.
                const f32 wx = tr * inv * size.x;
                const f32 wy = tu * inv * size.y;
                const Vector3f w{cam.up.x * wx - cam.right.x * wy,
                                 cam.up.y * wx - cam.right.y * wy,
                                 cam.up.z * wx - cam.right.z * wy};
                const Vector3f end{centre.x + tail.x, centre.y + tail.y, centre.z + tail.z};
                EmitStrip(out, {centre.x + w.x, centre.y + w.y, centre.z + w.z},
                          {centre.x - w.x, centre.y - w.y, centre.z - w.z},
                          {end.x + w.x, end.y + w.y, end.z + w.z},
                          {end.x - w.x, end.y - w.y, end.z - w.z}, vcol, normal, cell.u0,
                          cell.v0, cell.u1, cell.v1);
            } else {
                // Too short to point anywhere on screen. The client draws a
                // plain screen-aligned quad rather than dropping the tail, which
                // is where WC3 skips the particle instead.
                EmitQuad(out, centre, screenA, screenB, vcol, normal, cell.u0, cell.v0, cell.u1,
                         cell.v1);
            }
            appendExtraUV(tailFrom, cell.u0, cell.v0);
        }
    }

    return static_cast<i32>(out.size() - startSize);
}

} // namespace

// The twinkle table. The client fills `kTwinkleTableSize` floats with `rand()`-seeded noise when
// the particle system starts (`CParticleEmitter2::Init` @0x10169efe0), so its
// blink pattern genuinely differs between runs of the game. Ours is seeded
// fixed: same uniform [0,1) distribution, reproducible run to run — the one
// deliberate divergence in this file, and what lets a twinkling emitter be
// trace-gated at all.
const f32* TwinkleTable() {
    static const std::array<f32, kTwinkleTableSize> table = [] {
        std::array<f32, kTwinkleTableSize> t{};
        RndSeed s(kTwinkleTableSeed);
        for (f32& v : t)
            v = CRandom::real_(s);
        return t;
    }();
    return table.data();
}

u32 TwinkleIndex(u16 seed, f32 age, f32 twinkleSpeed) {
    const u8 phase = static_cast<u8>(static_cast<i32>(age * twinkleSpeed));
    return ((static_cast<u32>(seed) & kTwinkleSeedByteMask) + phase) & kTwinkleIndexMask;
}

i32 BuildEmitterGeometry(const Emitter2& emitter, const BuildGeometryInput& in,
                         std::vector<Vertex>& out) {
    if (!in.worldToView)
        return 0;
    if (emitter.Pool().AliveCount() == 0)
        return 0;
    // Same split as the two integrators: the dialect picks a whole builder, not
    // a set of branches inside one.
    return emitter.Behavior().renderRandomsFromSeed ? BuildWowGeometry(emitter, in, out)
                                                    : BuildWc3Geometry(emitter, in, out);
}

} // namespace whiteout::flakes::renderer::particle
