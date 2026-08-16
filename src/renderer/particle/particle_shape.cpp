#include "renderer/particle/particle_shape.h"

#include <cmath>

namespace whiteout::flakes::renderer::particle {

namespace {

// Shared cone direction, scaled by speed. The multiply order matters: WC3
// computes vx = speed*sin(lat) and then vy = vx*sin(long), so the speed factor
// enters *before* the longitude factor. Folding it into a unit direction and
// scaling afterwards is algebraically identical but not bitwise identical, and
// the trace diff sees the difference.
inline Vector3f ConeVelocity(f32 speed, f32 rotY, f32 rotZ) {
    f32 vx = speed * std::sin(rotY);
    f32 vz = speed * std::cos(rotY);

    f32 vy = vx * std::sin(rotZ);
    vx = vx * std::cos(rotZ);

    return {vx, vy, vz};
}

// Unit vector from `zSource` on the local Z axis through the spawn point. Used
// by every WoW generator as the "aim outward from a virtual origin" mode.
inline Vector3f AimFromZSource(const Vector3f& pos, f32 zSource) {
    Vector3f d = {pos.x, pos.y, pos.z - zSource};
    const f32 len2 = d.x * d.x + d.y * d.y + d.z * d.z;
    if (len2 <= 0.0f)
        return {0.0f, 0.0f, 1.0f};
    const f32 inv = 1.0f / std::sqrt(len2);
    return {d.x * inv, d.y * inv, d.z * inv};
}

} // namespace

void PlaneShape::Sample(SpawnSample& out, const SpawnParams& p, RndSeed& rnd) const {
    const f32 heightTerm = CRandom::reals_(rnd) * p.height * 0.5f;
    const f32 widthTerm = CRandom::reals_(rnd) * p.width * 0.5f;
    out.localPos = {widthTerm, heightTerm, 0.0f};

    const f32 rotY = p.latitude * CRandom::reals_(rnd);
    const f32 rotZ = p.longitude * CRandom::reals_(rnd);
    out.localVel = ConeVelocity(DrawSpeed(rnd, p), rotY, rotZ);
}

void ConeShape::Sample(SpawnSample& out, const SpawnParams& p, RndSeed& rnd) const {
    out.localPos = {0.0f, 0.0f, 0.0f};

    const f32 rotY = p.latitude * CRandom::reals_(rnd);
    const f32 rotZ = p.longitude * CRandom::reals_(rnd);
    out.localVel = ConeVelocity(DrawSpeed(rnd, p), rotY, rotZ);
}

// ---------------------------------------------------------------------------
// WoW generators
//
// Draw order is load-bearing and matches the decompiles exactly: position
// first, then the speed draw, then the two angle draws — and the angle draws
// are SKIPPED entirely in zSource mode, which is why that branch is not simply
// a different direction but a different stream position for everything after.
// ---------------------------------------------------------------------------

void WowPlaneShape::Sample(SpawnSample& out, const SpawnParams& p, RndSeed& rnd) const {
    out.localPos = {CRandom::reals_(rnd) * 0.5f * p.width,
                    CRandom::reals_(rnd) * 0.5f * p.height, 0.0f};

    const f32 speed = DrawSpeed(rnd, p);
    if (p.zSource <= kMinZSource) {
        const f32 polar = p.latitude * CRandom::reals_(rnd);
        const f32 azimuth = p.horizontalRange * CRandom::reals_(rnd);
        // Polar 0 points at +Z, unlike WC3's cone which builds from sin(lat).
        const f32 sp = std::sin(polar);
        out.localVel = {std::cos(azimuth) * sp * speed, std::sin(azimuth) * sp * speed,
                        std::cos(polar) * speed};
    } else {
        const Vector3f d = AimFromZSource(out.localPos, p.zSource);
        out.localVel = {d.x * speed, d.y * speed, d.z * speed};
    }
}

void WowSphereShape::Sample(SpawnSample& out, const SpawnParams& p, RndSeed& rnd) const {
    // The two area floats are min and max radius here.
    const f32 radius = p.width + CRandom::real_(rnd) * (p.height - p.width);
    const f32 polar = CRandom::reals_(rnd) * p.latitude;
    const f32 azimuth = CRandom::reals_(rnd) * p.horizontalRange;
    // Elevation, not inclination: polar 0 lies in the XY plane.
    const f32 cp = std::cos(polar);
    Vector3f dir = {std::cos(azimuth) * cp, std::sin(azimuth) * cp, std::sin(polar)};
    out.localPos = {dir.x * radius, dir.y * radius, dir.z * radius};

    const f32 speed = DrawSpeed(rnd, p);
    if (p.zSource > kMinZSource)
        dir = AimFromZSource(out.localPos, p.zSource);
    else if (hemisphereUp_)
        dir = {0.0f, 0.0f, 1.0f};
    out.localVel = {dir.x * speed, dir.y * speed, dir.z * speed};
}

void WowSplineShape::Evaluate(f32 t, Vector3f& pos, Vector3f& tangent) const {
    if (points_.empty()) {
        pos = {0.0f, 0.0f, 0.0f};
        tangent = {0.0f, 0.0f, 1.0f};
        return;
    }
    if (points_.size() == 1) {
        pos = points_[0];
        tangent = {0.0f, 0.0f, 1.0f};
        return;
    }
    const f32 spans = static_cast<f32>(points_.size() - 1);
    f32 u = t * spans;
    if (u < 0.0f)
        u = 0.0f;
    if (u > spans)
        u = spans;
    usize i = static_cast<usize>(u);
    if (i >= points_.size() - 1)
        i = points_.size() - 2;
    const f32 local = u - static_cast<f32>(i);
    const Vector3f& a = points_[i];
    const Vector3f& b = points_[i + 1];
    pos = {a.x + (b.x - a.x) * local, a.y + (b.y - a.y) * local, a.z + (b.z - a.z) * local};
    tangent = {b.x - a.x, b.y - a.y, b.z - a.z};
}

void WowSplineShape::Sample(SpawnSample& out, const SpawnParams& p, RndSeed& rnd) const {
    const f32 t0 = (p.width < 0.0f) ? 0.0f : ((p.width > 1.0f) ? 1.0f : p.width);
    const f32 t1 = (p.height < 0.0f) ? 0.0f : ((p.height > 1.0f) ? 1.0f : p.height);
    const f32 t = t0 + CRandom::real_(rnd) * (t1 - t0);

    Vector3f tangent;
    Evaluate(t, out.localPos, tangent);

    const f32 speed = DrawSpeed(rnd, p);
    if (p.zSource > kMinZSource) {
        const Vector3f d = AimFromZSource(out.localPos, p.zSource);
        out.localVel = {d.x * speed, d.y * speed, d.z * speed};
        return;
    }

    const f32 len2 = tangent.x * tangent.x + tangent.y * tangent.y + tangent.z * tangent.z;
    if (len2 > 0.0f) {
        const f32 inv = 1.0f / std::sqrt(len2);
        tangent = {tangent.x * inv, tangent.y * inv, tangent.z * inv};
    } else {
        tangent = {0.0f, 0.0f, 1.0f};
    }

    // The client rotates the tangent about itself, which is a no-op for the
    // direction — what it actually varies is the spread the two range tracks
    // describe. Reproduced as a cone about the tangent so the ranges do
    // something visible rather than being silently discarded.
    if (p.latitude != 0.0f) {
        const f32 polar = p.latitude * CRandom::reals_(rnd);
        const f32 azimuth =
            (p.horizontalRange != 0.0f) ? p.horizontalRange * CRandom::reals_(rnd) : 0.0f;
        // Any vector not parallel to the tangent gives a usable basis.
        Vector3f up = (std::abs(tangent.z) > 0.9f) ? Vector3f{1.0f, 0.0f, 0.0f}
                                                   : Vector3f{0.0f, 0.0f, 1.0f};
        Vector3f side = {up.y * tangent.z - up.z * tangent.y, up.z * tangent.x - up.x * tangent.z,
                         up.x * tangent.y - up.y * tangent.x};
        const f32 sl2 = side.x * side.x + side.y * side.y + side.z * side.z;
        if (sl2 > 0.0f) {
            const f32 si = 1.0f / std::sqrt(sl2);
            side = {side.x * si, side.y * si, side.z * si};
            const Vector3f bin = {tangent.y * side.z - tangent.z * side.y,
                                  tangent.z * side.x - tangent.x * side.z,
                                  tangent.x * side.y - tangent.y * side.x};
            const f32 sp = std::sin(polar), cpz = std::cos(polar);
            const f32 ca = std::cos(azimuth), sa = std::sin(azimuth);
            tangent = {tangent.x * cpz + (side.x * ca + bin.x * sa) * sp,
                       tangent.y * cpz + (side.y * ca + bin.y * sa) * sp,
                       tangent.z * cpz + (side.z * ca + bin.z * sa) * sp};
        }
    }
    out.localVel = {tangent.x * speed, tangent.y * speed, tangent.z * speed};
}

} // namespace whiteout::flakes::renderer::particle
