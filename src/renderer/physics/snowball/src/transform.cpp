#include "snowball/transform.h"

namespace snowball {

Mtx RotationMatrix(const Vec4& q) {
    const f32 xx = q.x * q.x;
    const f32 yy = q.y * q.y;
    const f32 zz = q.z * q.z;
    const f32 xy = q.x * q.y;
    const f32 xz = q.x * q.z;
    const f32 yz = q.y * q.z;
    const f32 wx = q.w * q.x;
    const f32 wy = q.w * q.y;
    const f32 wz = q.w * q.z;

    Mtx m;
    m.c[0] = {1.0f - 2.0f * (yy + zz), 2.0f * (xy + wz), 2.0f * (xz - wy), 0.0f};
    m.c[1] = {2.0f * (xy - wz), 1.0f - 2.0f * (xx + zz), 2.0f * (yz + wx), 0.0f};
    m.c[2] = {2.0f * (xz + wy), 2.0f * (yz - wx), 1.0f - 2.0f * (xx + yy), 0.0f};
    m.c[3] = constants::kUnitW;
    return m;
}

Vec4 QuatMultiply(const Vec4& a, const Vec4& b) {
    const Vec4 av{a.x, a.y, a.z, 0.0f};
    const Vec4 bv{b.x, b.y, b.z, 0.0f};
    const Vec4 v = bv * a.w + av * b.w + Cross3(av, bv);
    return {v.x, v.y, v.z, a.w * b.w - Dot3(av, bv)};
}

Vec4 QuatNormalize(const Vec4& q) {
    const f32 lengthSquared = (q.x * q.x + q.y * q.y) + (q.z * q.z + q.w * q.w);
    if (lengthSquared <= constants::kZeroSafe.x) {
        return constants::kQuatIdentity;
    }
    return q * Rsqrt(lengthSquared);
}

Vec4 IntegrateRotation(const Vec4& q, const Vec4& angularVelocity, f32 dt) {
    // dq/dt = 0.5 * omega * q with omega a pure quaternion in the world frame, then renormalise.
    // First order, which is what a fixed-step rigid-body integrator uses; the renormalisation is
    // what keeps it a rotation rather than a slowly growing scale.
    const Vec4 omega{angularVelocity.x, angularVelocity.y, angularVelocity.z, 0.0f};
    return QuatNormalize(q + QuatMultiply(omega, q) * (0.5f * dt));
}

Aabb TransformAabb(const Transform& xf, const Aabb& box) {
    const Mtx r = RotationMatrix(xf.rotation);
    const Vec4 centre = r.Transform(box.Centre()) + xf.position;

    // |R| * extent gives the tight bounds of the rotated box directly.
    const Vec4 e = box.Extent();
    const auto abs3 = [](const Vec4& v) {
        return Vec4{v.x < 0.0f ? -v.x : v.x, v.y < 0.0f ? -v.y : v.y, v.z < 0.0f ? -v.z : v.z};
    };
    const Vec4 extent = abs3(r.c[0]) * e.x + abs3(r.c[1]) * e.y + abs3(r.c[2]) * e.z;

    return Aabb{centre - extent, centre + extent};
}

}  // namespace snowball
