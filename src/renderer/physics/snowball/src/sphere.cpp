#include "snowball/sphere.h"

namespace snowball {

f32 Volume(const Sphere& s) {
    const f32 r2 = s.radius * s.radius;
    return (r2 * ((constants::kOneThird.x * constants::kTwo.x) * constants::kTwoPi.x)) * s.radius;
}

MassProperties ComputeMass(const Sphere& s, f32 density) {
    // 4pi/3 folded to a literal, and the multiply order is fixed: regrouping it moves the last
    // ULP of every sphere's mass, and downstream results are pinned to these exact bits.
    const f32 r2 = s.radius * s.radius;
    const f32 mass = (r2 * (density * 4.1887903f)) * s.radius;
    const f32 diagonal = r2 * (0.4f * mass);

    const Vec4& p = s.centre;
    const f32 px2 = p.x * p.x;
    const f32 py2 = p.y * p.y;
    const f32 pz2 = p.z * p.z;

    const f32 ixx = ((py2 + pz2) * mass) + diagonal;
    const f32 iyy = ((pz2 + px2) * mass) + diagonal;
    const f32 izz = ((px2 + py2) * mass) + diagonal;

    // The three off-diagonal terms are each associated differently, on purpose. They agree
    // algebraically but not always in the last bit, so keep the mixed groupings: unifying them
    // changes bits the numeric contract pins.
    const f32 pxm = p.x * mass;
    const f32 ixy = 0.0f - (pxm * p.y);
    const f32 ixz = 0.0f - (p.z * pxm);
    const f32 iyz = 0.0f - ((mass * p.y) * p.z);

    MassProperties out;
    out.inertia.c[0] = {ixx, ixy, ixz, 0.0f};
    out.inertia.c[1] = {ixy, iyy, iyz, 0.0f};
    out.inertia.c[2] = {ixz, iyz, izz, 0.0f};
    out.centre = p;
    out.mass = mass;
    return out;
}

bool TestOverlap(const Sphere& a, const Sphere& b) {
    const Vec4 d = b.centre - a.centre;
    const f32 reach = a.radius + b.radius;
    return LengthSquared3(d) <= reach * reach;
}

}  // namespace snowball
