#include "renderer/particle/particle_geometry.h"
#include "whiteout/flakes/util/coordinate_system.h"

#include <algorithm>
#include <cmath>

namespace whiteout::flakes::renderer::particle {

namespace {

constexpr f32 kEpsilon = 1e-6f;

constexpr f32 vc[4][2] = {
    { -1.0f,  1.0f },
    { -1.0f, -1.0f },
    {  1.0f,  1.0f },
    {  1.0f, -1.0f },
};

struct CameraBasis {
    Vector3f fwd;
    Vector3f right;
    Vector3f up;
};

CameraBasis BasisFromView(const Matrix44f& viewMatrix) {
    CameraBasis b{};

    b.right = { viewMatrix.data[0][0], viewMatrix.data[1][0], viewMatrix.data[2][0] };
    b.up    = { viewMatrix.data[0][1], viewMatrix.data[1][1], viewMatrix.data[2][1] };

    b.fwd   = { -viewMatrix.data[0][2], -viewMatrix.data[1][2], -viewMatrix.data[2][2] };
    return b;
}

inline Vector3f Cross(const Vector3f& a, const Vector3f& b) {
    return { a.y * b.z - a.z * b.y,
             a.z * b.x - a.x * b.z,
             a.x * b.y - a.y * b.x };
}

inline f32 Dot(const Vector3f& a, const Vector3f& b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

inline f32 LengthSq(const Vector3f& v) { return v.x*v.x + v.y*v.y + v.z*v.z; }

inline Vector3f Normalize(const Vector3f& v, const Vector3f& fallback = {0,0,1}) {
    f32 l2 = LengthSq(v);
    if (l2 < kEpsilon) return fallback;
    f32 inv = 1.0f / std::sqrt(l2);
    return { v.x * inv, v.y * inv, v.z * inv };
}

void CellToUV(const Emitter2& e, i32 cell, f32& u, f32& v) {
    u32 cols = e.TextureCols();
    if (cols == 0) cols = 1;
    u32 rows = e.TextureRows();
    if (rows == 0) rows = 1;

    u32 col, row;
    if ((cols & (cols - 1)) == 0) {

        col = static_cast<u32>(cell) & (cols - 1);
        row = static_cast<u32>(cell) >> e.TextureLog();
    } else {
        col = static_cast<u32>(cell) % cols;
        row = static_cast<u32>(cell) / cols;
    }
    u = col * e.OoTextureWidth();
    v = row * e.OoTextureHeight();
}

ImVector CombineColors(ImVector a, ImVector b) {
    return {
        static_cast<u8>((a.a * b.a) / 255),
        static_cast<u8>((a.r * b.r) / 255),
        static_cast<u8>((a.g * b.g) / 255),
        static_cast<u8>((a.b * b.b) / 255),
    };
}

struct SortRecord {
    u32 aliveIndex;
    f32 viewZ;
};

}

i32 BuildEmitterGeometry(const Emitter2& emitter,
                         const BuildGeometryInput& in,
                         std::vector<Vertex>& out)
{
    if (!in.worldToView) return 0;
    const ParticlePool& pool = emitter.Pool();
    if (pool.AliveCount() == 0) return 0;

    const bool hasHead = emitter.HasHead();
    const bool hasTail = emitter.HasTail();
    if (!hasHead && !hasTail) return 0;

    const bool modelSpace = emitter.UseModelSpace();
    const bool xyQuads    = emitter.XYQuads();
    const f32 angVel      = emitter.AngularVelocity();
    const bool useAngVel  = std::abs(angVel) > kEpsilon;
    const f32 tailLength  = emitter.TailLength();
    const ParticleMaterialDesc& mat = emitter.Material();

    CameraBasis cam = BasisFromView(*in.worldToView);

    const CoordSpace emSpace  = emitter.GetCoordSpace();
    const bool needsConvert   = (emSpace != CoordinateSystem::Default());

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

    std::vector<SortRecord> order;
    order.reserve(pool.AliveCount());
    for (usize i = 0; i < pool.AliveCount(); ++i) {
        u32 idx = pool.AliveAt(i);
        const Particle2& p = pool[idx];
        SortRecord rec{ static_cast<u32>(i), 0.0f };
        if (emitter.SortZ()) {
            Vector3f wp, wv;
            resolveWorld(p, wp, wv);
            rec.viewZ = Dot(wp, cam.fwd);
        }
        order.push_back(rec);
    }
    if (emitter.SortZ()) {

        std::sort(order.begin(), order.end(),
                  [](const SortRecord& a, const SortRecord& b) { return a.viewZ > b.viewZ; });
    }

    const usize startSize = out.size();
    const Vector3f normal = {0.0f, 0.0f, 1.0f};

    for (const SortRecord& rec : order) {
        u32 idx = pool.AliveAt(rec.aliveIndex);
        const Particle2& p = pool[idx];

        i32 kf = static_cast<i32>(p.keyFrame);
        if (kf < 0) kf = 0;
        if (kf > 1) kf = 1;
        f32 prevEnd = (kf == 0) ? 0.0f : emitter.Key(0).endTime;
        ImVector baseColor;
        i32 headCell = 0, tailCell = 0;
        f32 scale = 0.0f;
        emitter.Key(kf).Interpolate(p.age, prevEnd, baseColor, headCell, tailCell, scale);

        ImVector color = baseColor;
        if (in.fogEnabled && !mat.unfogged && in.fogSampler) {
            Vector3f wp, _wv;
            resolveWorld(p, wp, _wv);
            ImVector fog = in.fogSampler(wp);
            color = CombineColors(baseColor, fog);
            color.a = baseColor.a;
        }

        Vector4f vcol = color.ToVec4();

        const f32 corner = scale;

        Vector3f worldPos, worldVel;
        resolveWorld(p, worldPos, worldVel);

        if (hasHead) {
            f32 u0, vq0, u1, vq1;
            {
                f32 cellU, cellV;
                CellToUV(emitter, headCell, cellU, cellV);
                u0 = cellU; vq0 = cellV;
                u1 = cellU + emitter.OoTextureWidth();
                vq1 = cellV + emitter.OoTextureHeight();
            }

            Vector3f right = cam.right;
            Vector3f up    = cam.up;

            if (useAngVel) {

                f32 theta = p.age * angVel;
                f32 ct = std::cos(theta), st = std::sin(theta);
                Vector3f r2 = { right.x * ct + up.x * st,
                                right.y * ct + up.y * st,
                                right.z * ct + up.z * st };
                Vector3f u2 = { up.x * ct - right.x * st,
                                up.y * ct - right.y * st,
                                up.z * ct - right.z * st };
                right = r2; up = u2;
            } else if (xyQuads) {

                Vector3f localVelXY  = { p.velocity.x,  p.velocity.y, 0.0f };
                Vector3f localPerpXY = { p.velocity.y, -p.velocity.x, 0.0f };

                Vector3f worldPvel     = localVelXY;
                Vector3f worldPvelPerp = localPerpXY;
                if (modelSpace) {
                    worldPvel     = whiteout::transform_normal(localVelXY,  emitter.ModelToWorld());
                    worldPvelPerp = whiteout::transform_normal(localPerpXY, emitter.ModelToWorld());
                }
                if (needsConvert) {
                    worldPvel     = CoordinateSystem::ToDefaultDir(emSpace, worldPvel);
                    worldPvelPerp = CoordinateSystem::ToDefaultDir(emSpace, worldPvelPerp);
                }

                f32 mag2 = LengthSq(worldPvel);
                if (mag2 > kEpsilon) {

                    right = Normalize(worldPvelPerp);
                    up    = Normalize(worldPvel);
                }

            }

            Vector3f corners[4];
            for (i32 c = 0; c < 4; ++c) {
                f32 sx = vc[c][0] * corner;
                f32 sy = vc[c][1] * corner;
                corners[c] = {
                    worldPos.x + right.x * sx + up.x * sy,
                    worldPos.y + right.y * sx + up.y * sy,
                    worldPos.z + right.z * sx + up.z * sy
                };
            }

            const Vector2f uv[4] = {
                {u0, vq0}, {u0, vq1}, {u1, vq0}, {u1, vq1}
            };
            out.push_back({ corners[0], normal, vcol, uv[0] });
            out.push_back({ corners[1], normal, vcol, uv[1] });
            out.push_back({ corners[2], normal, vcol, uv[2] });
            out.push_back({ corners[3], normal, vcol, uv[3] });
            out.push_back({ corners[2], normal, vcol, uv[2] });
            out.push_back({ corners[1], normal, vcol, uv[1] });
        }

        if (hasTail) {
            f32 u0, vq0, u1, vq1;
            {
                f32 cellU, cellV;
                CellToUV(emitter, tailCell, cellU, cellV);
                u0 = cellU; vq0 = cellV;
                u1 = cellU + emitter.OoTextureWidth();
                vq1 = cellV + emitter.OoTextureHeight();
            }

            Vector3f negVel = { -worldVel.x * tailLength,
                                -worldVel.y * tailLength,
                                -worldVel.z * tailLength };
            if (LengthSq(negVel) < kEpsilon) continue;

            Vector3f tailEnd = { worldPos.x + negVel.x,
                                 worldPos.y + negVel.y,
                                 worldPos.z + negVel.z };

            Vector3f tailDir = Normalize(negVel);
            Vector3f perp = Cross(tailDir, cam.fwd);
            if (LengthSq(perp) < kEpsilon) {

                Vector3f altUp = (std::abs(tailDir.z) > 0.999f) ? Vector3f{0,1,0} : Vector3f{0,0,1};
                perp = Cross(tailDir, altUp);
            }
            perp = Normalize(perp);

            Vector3f w = { perp.x * corner, perp.y * corner, perp.z * corner };

            Vector3f hl = { worldPos.x - w.x, worldPos.y - w.y, worldPos.z - w.z };
            Vector3f hr = { worldPos.x + w.x, worldPos.y + w.y, worldPos.z + w.z };
            Vector3f tl = { tailEnd.x - w.x,  tailEnd.y - w.y,  tailEnd.z - w.z  };
            Vector3f tr = { tailEnd.x + w.x,  tailEnd.y + w.y,  tailEnd.z + w.z  };

            const Vector2f uvHL{u0, vq0};
            const Vector2f uvHR{u0, vq1};
            const Vector2f uvTL{u1, vq0};
            const Vector2f uvTR{u1, vq1};

            out.push_back({ hl, normal, vcol, uvHL });
            out.push_back({ hr, normal, vcol, uvHR });
            out.push_back({ tl, normal, vcol, uvTL });
            out.push_back({ tr, normal, vcol, uvTR });
            out.push_back({ tl, normal, vcol, uvTL });
            out.push_back({ hr, normal, vcol, uvHR });
        }
    }

    return static_cast<i32>(out.size() - startSize);
}

}
