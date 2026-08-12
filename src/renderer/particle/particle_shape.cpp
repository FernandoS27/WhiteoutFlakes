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

} // namespace whiteout::flakes::renderer::particle
