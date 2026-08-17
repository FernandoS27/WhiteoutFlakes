#include "snowball/capsule.h"

#include "snowball/math.h"
#include "snowball/transform.h"

#include <cmath>

namespace snowball {

f32 Volume(const Capsule& c) {
    const f32 r2 = c.radius * c.radius;
    const f32 length = std::sqrt(LengthSquared3(c.p2 - c.p1));
    const f32 cylinder = (r2 * constants::kPi.x) * length;
    const f32 caps =
        (((constants::kTwo.x * constants::kTwoPi.x) * constants::kOneThird.x) * r2) * c.radius;
    return cylinder + caps;
}

MassProperties ComputeMass(const Capsule& c, f32 density) {
    const Vec4 d = c.p2 - c.p1;
    const f32 lengthSquared = LengthSquared3(d);
    const f32 length = std::sqrt(lengthSquared);
    // A degenerate capsule -- both endpoints coincident -- is a sphere someone authored badly,
    // and letting it through would divide by zero below. Giving it no mass instead matches what
    // a zero-density fixture already does and keeps the NaN out of the solver.
    if (length <= constants::kEpsilon.x) {
        return {};
    }

    const f32 r2 = c.radius * c.radius;
    // See the header: no pi on the caps. Deliberate, and part of the numeric contract.
    const f32 capMass = ((c.radius * r2) * 0.66666669f) * density;
    const f32 cylinderMass = ((3.1415927f * r2) * length) * density;
    const f32 mass = (capMass + capMass) + cylinderMass;

    // Local diagonal, built with the capsule lying along +Y.
    const f32 axial = ((0.40000001f * capMass) * r2 + (0.40000001f * capMass) * r2) +
                      (0.5f * cylinderMass) * r2;
    // A hemisphere's centroid is 3r/8 from its flat face, which sits L/2 from the centre.
    const f32 capOffset = (c.radius * 0.375f) + (length * 0.5f);
    const f32 capTransverse =
        ((capMass * 0.25937501f) * r2) + ((capOffset * capOffset) * capMass);
    const f32 cylinderTransverse =
        ((0.25f * r2) + ((length * length) * 0.083333336f)) * cylinderMass;
    const f32 transverse = (capTransverse + capTransverse) + cylinderTransverse;

    // The rotation that carries +Y onto the segment, via the half-vector trick: for unit `u` and
    // `v`, `h = normalize(u + v)` bisects them, and `(cross(u, v), dot(u, v))` built from the
    // bisector is the half-angle quaternion. `h` is left unnormalised here and the quaternion is
    // normalised instead, which is the same rotation and one square root cheaper.
    const Vec4 axis = d * Rsqrt(lengthSquared);
    const Vec4 h = (axis + constants::kUnitY) * constants::kOneHalf;
    const Vec4 cross = Cross3(constants::kUnitY, h);
    Vec4 q{cross.x, cross.y, cross.z, Dot3(constants::kUnitY, h)};

    const f32 qLengthSquared = (q.x * q.x + q.y * q.y) + (q.z * q.z + q.w * q.w);
    if (qLengthSquared > constants::kZeroSafe.x) {
        q = q * Rsqrt(qLengthSquared);
    } else {
        // `axis == -Y`: the bisector vanishes and the rotation is a half turn about any axis
        // perpendicular to Y. The general perpendicular-picking logic, fed the constant +Y
        // reference, always lands on this one answer, so the whole choice is folded down to a
        // single literal -- there is no live branch to take here.
        q = Vec4{0.0f, 0.0f, -1.0f, 0.0f};
    }

    const Vec4 centre = (c.p1 + c.p2) * constants::kOneHalf;
    const Mtx r = RotationMatrix(q);

    // `R * diag(transverse, axial, transverse) * R^T`, a row of the left product at a time. The
    // diagonal collapses the first multiply to a scale per column, which is why only three
    // constants appear rather than a full 3x3 concat.
    const Vec4 rd0{transverse * r.c[0].x, axial * r.c[1].x, transverse * r.c[2].x, 0.0f};
    const Vec4 rd1{transverse * r.c[0].y, axial * r.c[1].y, transverse * r.c[2].y, 0.0f};
    const Vec4 rd2{transverse * r.c[0].z, axial * r.c[1].z, transverse * r.c[2].z, 0.0f};

    const Vec4 row0 = r.c[0] * rd0.x + (r.c[1] * rd0.y + r.c[2] * rd0.z);
    const Vec4 row1 = r.c[0] * rd1.x + (r.c[1] * rd1.y + r.c[2] * rd1.z);
    const Vec4 row2 = r.c[0] * rd2.x + (r.c[1] * rd2.y + r.c[2] * rd2.z);

    // Parallel axis onto the body origin, with each term's association fixed as written -- the
    // same shape as `ComputeMass(const Sphere&)`, where the three off-diagonals also each group
    // differently on purpose.
    const f32 cx2 = centre.x * centre.x;
    const f32 cy2 = centre.y * centre.y;
    const f32 cz2 = centre.z * centre.z;
    const f32 ixx = ((cy2 + cz2) * mass) + row0.x;
    const f32 iyy = row1.y + ((cz2 + cx2) * mass);
    const f32 izz = row2.z + ((cx2 + cy2) * mass);
    const f32 mcx = mass * centre.x;
    const f32 ixy = row0.y - (mcx * centre.y);
    const f32 ixz = row0.z - (centre.z * mcx);
    const f32 iyz = row1.z - ((mass * centre.y) * centre.z);

    MassProperties out;
    out.inertia.c[0] = {ixx, ixy, ixz, 0.0f};
    out.inertia.c[1] = {ixy, iyy, iyz, 0.0f};
    out.inertia.c[2] = {ixz, iyz, izz, 0.0f};
    out.centre = centre;
    out.mass = mass;
    return out;
}

}  // namespace snowball
