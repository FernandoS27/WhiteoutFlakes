#include "shape_geometry.hpp"

#include <algorithm>
#include <cmath>

namespace whiteout::cornflakes {
namespace {

constexpr f32 kPi = 3.14159265358979323846F;
[[maybe_unused]] constexpr f32 kUnusedPi = kPi;

f32 length2(const ShapePoint& p) noexcept { return p[0] * p[0] + p[1] * p[1] + p[2] * p[2]; }
f32 length3(const ShapePoint& p) noexcept { return std::sqrt(length2(p)); }
f32 radialLength(const ShapePoint& p) noexcept { return std::sqrt(p[0] * p[0] + p[1] * p[1]); }

ShapePoint rescale(const ShapePoint& p, f32 target) noexcept {
    const f32 len = length3(p);
    if (len <= 1e-20F) {
        return {target, 0.0F, 0.0F};
    }
    const f32 k = target / len;
    return {p[0] * k, p[1] * k, p[2] * k};
}

ShapePoint rescaleRadial(const ShapePoint& p, f32 target, f32 z) noexcept {
    const f32 len = radialLength(p);
    if (len <= 1e-20F) {
        return {target, 0.0F, z};
    }
    const f32 k = target / len;
    return {p[0] * k, p[1] * k, z};
}

f32 sphereField(const ShapePoint& p, f32 r, f32 ir) noexcept {
    const f32 d = length3(p);
    const f32 outer = d - r;
    return ir > 0.0F ? std::max(outer, ir - d) : outer;
}

f32 boxField(const ShapePoint& p, const ShapePoint& e) noexcept {
    const ShapePoint q{std::fabs(p[0]) - e[0], std::fabs(p[1]) - e[1], std::fabs(p[2]) - e[2]};
    const ShapePoint outside{std::max(q[0], 0.0F), std::max(q[1], 0.0F), std::max(q[2], 0.0F)};
    const f32 inside = std::min(std::max(q[0], std::max(q[1], q[2])), 0.0F);
    return length3(outside) + inside;
}

f32 cylinderSolidField(const ShapePoint& p, f32 r, f32 h) noexcept {
    const f32 dr = radialLength(p) - r;
    const f32 dz = std::fabs(p[2]) - 0.5F * h;
    const f32 outside =
        std::sqrt(std::max(dr, 0.0F) * std::max(dr, 0.0F) + std::max(dz, 0.0F) * std::max(dz, 0.0F));
    return outside + std::min(std::max(dr, dz), 0.0F);
}

f32 capsuleSolidField(const ShapePoint& p, f32 r, f32 h) noexcept {
    const f32 half = 0.5F * h;
    const f32 dz = std::clamp(p[2], -half, half);
    const ShapePoint q{p[0], p[1], p[2] - dz};
    return length3(q) - r;
}

f32 coneSolidField(const ShapePoint& p, f32 r, f32 h) noexcept {
    if (r <= 0.0F || h <= 0.0F) {
        return length3(p);
    }
    const f32 q0 = radialLength(p);
    const f32 q1 = p[2];
    const f32 ex = -r;
    const f32 ey = h;
    const f32 w0 = q0 - r;
    const f32 w1 = q1;
    const f32 e2 = ex * ex + ey * ey;
    const f32 t = std::clamp((w0 * ex + w1 * ey) / e2, 0.0F, 1.0F);
    const f32 s0 = w0 - ex * t;
    const f32 s1 = w1 - ey * t;
    const f32 slant = std::sqrt(s0 * s0 + s1 * s1);
    const f32 b0 = std::max(q0 - r, 0.0F);
    const f32 b1 = q1;
    const f32 base = std::sqrt(b0 * b0 + b1 * b1);
    const f32 dist = std::min(slant, base);
    const bool inside = q1 >= 0.0F && q1 <= h && q0 * h + q1 * r <= r * h;
    return inside ? -dist : dist;
}

f32 solidField(const SamplerShape& shape, const ShapePoint& p, f32 radius) noexcept {
    switch (shape.type) {
    case ShapeType::Box:
        return boxField(p, {shape.boxDimensions[0] * 0.5F, shape.boxDimensions[1] * 0.5F,
                            shape.boxDimensions[2] * 0.5F});
    case ShapeType::Sphere:
    case ShapeType::ComplexEllipsoid:
        return length3(p) - radius;
    case ShapeType::Cylinder:
        return cylinderSolidField(p, radius, shape.height);
    case ShapeType::Capsule:
        return capsuleSolidField(p, radius, shape.height);
    case ShapeType::Cone:
        return coneSolidField(p, radius, shape.height);
    case ShapeType::Mesh:
        break;
    }
    return length3(p);
}

}

bool shapeContains(const SamplerShape& shape, const ShapePoint& local) noexcept {
    if (shape.type == ShapeType::Mesh) {
        return false;
    }
    if (solidField(shape, local, shape.radius) >= 0.0F) {
        return false;
    }
    if (shape.innerRadius > 0.0F && solidField(shape, local, shape.innerRadius) <= 0.0F) {
        return false;
    }
    if (shape.type == ShapeType::ComplexEllipsoid && shape.hemisphere && local[2] < 0.0F) {
        return false;
    }
    return true;
}

f32 shapeDistanceField(const SamplerShape& shape, const ShapePoint& local) noexcept {
    if (shape.type == ShapeType::Mesh) {
        return 0.0F;
    }
    if (shape.type == ShapeType::Sphere || shape.type == ShapeType::ComplexEllipsoid) {
        return sphereField(local, shape.radius, shape.innerRadius);
    }
    const f32 outer = solidField(shape, local, shape.radius);
    if (shape.innerRadius <= 0.0F) {
        return outer;
    }
    return std::max(outer, -solidField(shape, local, shape.innerRadius));
}

ShapePoint shapeProject(const SamplerShape& shape, const ShapePoint& local) noexcept {
    const f32 r = shape.radius;
    const f32 ir = shape.innerRadius;
    const f32 h = shape.height;
    switch (shape.type) {
    case ShapeType::Sphere:
    case ShapeType::ComplexEllipsoid: {
        const f32 d = length3(local);
        const f32 shell = (ir > 0.0F && std::fabs(d - ir) < std::fabs(d - r)) ? ir : r;
        auto p = rescale(local, shell);
        if (shape.type == ShapeType::ComplexEllipsoid && shape.hemisphere && p[2] < 0.0F) {
            p[2] = -p[2];
        }
        return p;
    }
    case ShapeType::Box: {
        const ShapePoint e{shape.boxDimensions[0] * 0.5F, shape.boxDimensions[1] * 0.5F,
                           shape.boxDimensions[2] * 0.5F};
        ShapePoint p{std::clamp(local[0], -e[0], e[0]), std::clamp(local[1], -e[1], e[1]),
                     std::clamp(local[2], -e[2], e[2])};
        if (p[0] == local[0] && p[1] == local[1] && p[2] == local[2]) {
            u32 best = 0U;
            f32 bestGap = e[0] - std::fabs(p[0]);
            for (u32 c = 1U; c < 3U; ++c) {
                const f32 gap = e[c] - std::fabs(p[c]);
                if (gap < bestGap) {
                    bestGap = gap;
                    best = c;
                }
            }
            p[best] = p[best] < 0.0F ? -e[best] : e[best];
        }
        return p;
    }
    case ShapeType::Cylinder: {
        const f32 half = 0.5F * h;
        const f32 rad = radialLength(local);
        const f32 shell = (ir > 0.0F && std::fabs(rad - ir) < std::fabs(rad - r)) ? ir : r;
        const f32 z = std::clamp(local[2], -half, half);
        if (std::fabs(local[2]) > half && rad < r && rad > ir) {
            return {local[0], local[1], local[2] < 0.0F ? -half : half};
        }
        return rescaleRadial(local, shell, z);
    }
    case ShapeType::Capsule: {
        const f32 half = 0.5F * h;
        const f32 zc = std::clamp(local[2], -half, half);
        const ShapePoint axisToP{local[0], local[1], local[2] - zc};
        const f32 shell =
            (ir > 0.0F && std::fabs(length3(axisToP) - ir) < std::fabs(length3(axisToP) - r)) ? ir
                                                                                             : r;
        const auto dir = rescale(axisToP, shell);
        return {dir[0], dir[1], dir[2] + zc};
    }
    case ShapeType::Cone: {
        if (r <= 0.0F || h <= 0.0F) {
            return {0.0F, 0.0F, 0.0F};
        }
        const f32 q0 = radialLength(local);
        const f32 q1 = local[2];
        const f32 ex = -r;
        const f32 ey = h;
        const f32 t = std::clamp(((q0 - r) * ex + q1 * ey) / (ex * ex + ey * ey), 0.0F, 1.0F);
        const f32 slantR = r + ex * t;
        const f32 slantZ = ey * t;
        const f32 slantD = (q0 - slantR) * (q0 - slantR) + (q1 - slantZ) * (q1 - slantZ);
        const f32 baseR = std::min(q0, r);
        const f32 baseD = (q0 - baseR) * (q0 - baseR) + q1 * q1;
        return baseD < slantD ? rescaleRadial(local, baseR, 0.0F)
                              : rescaleRadial(local, slantR, slantZ);
    }
    case ShapeType::Mesh:
        break;
    }
    return local;
}

ShapePoint shapeSurfaceNormal(const SamplerShape& shape, const ShapePoint& local) noexcept {
    if (shape.type == ShapeType::Mesh) {
        return {0.0F, 0.0F, 1.0F};
    }
    const f32 scale = std::max({shape.radius, shape.height, shape.boxDimensions[0],
                                shape.boxDimensions[1], shape.boxDimensions[2], 1.0F});
    const f32 e = scale * 1e-3F;
    const f32 dx = shapeDistanceField(shape, {local[0] + e, local[1], local[2]}) -
                   shapeDistanceField(shape, {local[0] - e, local[1], local[2]});
    const f32 dy = shapeDistanceField(shape, {local[0], local[1] + e, local[2]}) -
                   shapeDistanceField(shape, {local[0], local[1] - e, local[2]});
    const f32 dz = shapeDistanceField(shape, {local[0], local[1], local[2] + e}) -
                   shapeDistanceField(shape, {local[0], local[1], local[2] - e});
    const ShapePoint g{dx, dy, dz};
    const f32 len = length3(g);
    if (len <= 1e-20F) {
        return {0.0F, 0.0F, 1.0F};
    }
    return {g[0] / len, g[1] / len, g[2] / len};
}

bool shapeIntersect(const SamplerShape& shape, const ShapePoint& origin,
                    const ShapePoint& direction, f32 length, bool twoSided, f32& outT) noexcept {
    if (shape.type == ShapeType::Mesh) {
        return false;
    }
    const ShapePoint seg{direction[0] * length, direction[1] * length, direction[2] * length};
    const f32 segLen = length3(seg);
    if (segLen <= 0.0F) {
        return false;
    }

    const bool solidSphere =
        (shape.type == ShapeType::Sphere || shape.type == ShapeType::ComplexEllipsoid) &&
        shape.innerRadius <= 0.0F && !(shape.type == ShapeType::ComplexEllipsoid && shape.hemisphere);
    if (solidSphere) {
        const f32 a = length2(seg);
        const f32 b = 2.0F * (origin[0] * seg[0] + origin[1] * seg[1] + origin[2] * seg[2]);
        const f32 c = length2(origin) - shape.radius * shape.radius;
        const f32 disc = b * b - 4.0F * a * c;
        if (disc < 0.0F) {
            return false;
        }
        const f32 sq = std::sqrt(disc);
        const f32 t0 = (-b - sq) / (2.0F * a);
        const f32 t1 = (-b + sq) / (2.0F * a);
        const f32 t = (t0 >= 0.0F) ? t0 : (twoSided ? t1 : -1.0F);
        if (t < 0.0F || t > 1.0F) {
            return false;
        }
        outT = t;
        return true;
    }

    const bool startInside = shapeContains(shape, origin);
    if (startInside && !twoSided) {
        return false;
    }
    f32 t = 0.0F;
    for (int step = 0; step < 192; ++step) {
        const ShapePoint p{origin[0] + seg[0] * t, origin[1] + seg[1] * t, origin[2] + seg[2] * t};
        f32 d = shapeDistanceField(shape, p);
        if (startInside) {
            d = -d;
        }
        const f32 tol = std::max(1e-5F, segLen * 1e-5F);
        if (d < tol) {
            outT = t;
            return t <= 1.0F;
        }
        t += d / segLen;
        if (t > 1.0F) {
            return false;
        }
    }
    return false;
}

}
