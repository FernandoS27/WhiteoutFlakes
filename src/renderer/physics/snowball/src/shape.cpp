#include "snowball/shape.h"

#include <algorithm>
#include <cmath>
#include <initializer_list>

namespace snowball {
namespace {

Aabb Bounds(std::initializer_list<Vec4> points, f32 radius) {
    Aabb box{Vec4::Splat(constants::kMaxFloat.x), Vec4::Splat(-constants::kMaxFloat.x)};
    for (const Vec4& p : points) {
        box.lower = {std::min(box.lower.x, p.x), std::min(box.lower.y, p.y),
                     std::min(box.lower.z, p.z)};
        box.upper = {std::max(box.upper.x, p.x), std::max(box.upper.y, p.y),
                     std::max(box.upper.z, p.z)};
    }
    const Vec4 skin = Vec4::Splat(radius);
    return Aabb{box.lower - skin, box.upper + skin};
}

Vec4 ToWorld(const Transform& xf, const Vec4& p) {
    return RotationMatrix(xf.rotation).Transform(p) + xf.position;
}

}  // namespace

ShapeType TypeOf(const Shape& shape) {
    return std::visit(
        [](const auto& s) {
            using T = std::decay_t<decltype(s)>;
            if constexpr (std::is_same_v<T, Sphere>) {
                return ShapeType::Sphere;
            } else if constexpr (std::is_same_v<T, Capsule>) {
                return ShapeType::Capsule;
            } else if constexpr (std::is_same_v<T, Triangle>) {
                return ShapeType::Triangle;
            } else if constexpr (std::is_same_v<T, TreeMesh>) {
                return ShapeType::TreeMesh;
            } else if constexpr (std::is_same_v<T, HeightField>) {
                return ShapeType::HeightField;
            } else {
                return ShapeType::Polytope;
            }
        },
        shape);
}

f32 Margin(const Shape& shape) {
    return std::visit(
        [](const auto& s) {
            using T = std::decay_t<decltype(s)>;
            if constexpr (std::is_same_v<T, Polytope>) {
                return s.margin;
            } else if constexpr (std::is_same_v<T, TreeMesh> ||
                                 std::is_same_v<T, HeightField>) {
                // The margin is a property of the code path, not the mesh: each queried
                // triangle carries its own 0.01 skin at collide time, and the mesh object's
                // radius stays at its zero-fill.
                return 0.0f;
            } else {
                return s.radius;
            }
        },
        shape);
}

Vec4 ShapeCentroid(const Shape& shape) {
    return std::visit(
        [](const auto& s) -> Vec4 {
            using T = std::decay_t<decltype(s)>;
            if constexpr (std::is_same_v<T, Sphere>) {
                return s.centre;
            } else if constexpr (std::is_same_v<T, Capsule>) {
                return (s.p1 + s.p2) * 0.5f;
            } else if constexpr (std::is_same_v<T, Polytope>) {
                return s.centroid;
            } else if constexpr (std::is_same_v<T, Triangle>) {
                return Centroid(s);
            } else {
                return Vec4{};
            }
        },
        shape);
}

f32 Volume(const Shape& shape) {
    return std::visit(
        [](const auto& s) {
            using T = std::decay_t<decltype(s)>;
            if constexpr (std::is_same_v<T, Sphere> || std::is_same_v<T, Capsule> ||
                          std::is_same_v<T, Polytope>) {
                return Volume(s);
            } else {
                return 0.0f;
            }
        },
        shape);
}

Shape TransformShape(const Shape& shape, const Transform& xf) {
    const Mtx rotation = RotationMatrix(xf.rotation);
    return std::visit(
        [&](const auto& s) -> Shape {
            using T = std::decay_t<decltype(s)>;
            T out = s;
            if constexpr (std::is_same_v<T, TreeMesh> || std::is_same_v<T, HeightField>) {
                // Unreachable: a mesh contact runs in the mesh's own frame and applies the
                // transform to the *results* (mesh_contact.cpp), so nothing ever asks for a
                // world-space copy of the triangles.
                return out;
            } else if constexpr (std::is_same_v<T, Sphere>) {
                out.centre = ToWorld(xf, s.centre);
            } else if constexpr (std::is_same_v<T, Capsule>) {
                out.p1 = ToWorld(xf, s.p1);
                out.p2 = ToWorld(xf, s.p2);
            } else if constexpr (std::is_same_v<T, Triangle>) {
                out.v1 = ToWorld(xf, s.v1);
                out.v2 = ToWorld(xf, s.v2);
                out.v3 = ToWorld(xf, s.v3);
            } else {
                for (Vec4& v : out.hull.vertices) {
                    v = ToWorld(xf, v);
                }
                for (HullFace& face : out.hull.faces) {
                    face.normal = rotation.Transform(face.normal);
                    // The plane offset has to be re-derived rather than rotated: it is a
                    // distance from the origin, and the origin moved.
                    face.offset = Dot3(face.normal,
                                       out.hull.vertices[static_cast<usize>(
                                           out.hull.edges[static_cast<usize>(face.firstEdge)]
                                               .origin)]);
                }
                out.centroid = ToWorld(xf, s.centroid);
            }
            return out;
        },
        shape);
}

Aabb ComputeAabb(const Shape& shape, const Transform& xf) {
    // One rule for every shape: bound the *core* geometry, then expand by the shape's margin.
    // A sphere's core is its centre and a capsule's is its segment, so their radius comes back
    // through the margin rather than being special-cased -- and a polytope's 0.01 skin is
    // included on the same footing, so a half-0.5 box bounds one skin wider than a radius-0.5
    // sphere on every axis.
    const f32 margin = Margin(shape);
    return std::visit(
        [&xf, margin](const auto& s) {
            using T = std::decay_t<decltype(s)>;
            if constexpr (std::is_same_v<T, Sphere>) {
                return Bounds({ToWorld(xf, s.centre)}, margin);
            } else if constexpr (std::is_same_v<T, Capsule>) {
                return Bounds({ToWorld(xf, s.p1), ToWorld(xf, s.p2)}, margin);
            } else if constexpr (std::is_same_v<T, Triangle>) {
                return Bounds({ToWorld(xf, s.v1), ToWorld(xf, s.v2), ToWorld(xf, s.v3)}, margin);
            } else if constexpr (std::is_same_v<T, TreeMesh>) {
                return s.GetAabb(xf, margin);
            } else if constexpr (std::is_same_v<T, HeightField>) {
                return HeightFieldAabb(s, xf, margin);
            } else {
                Aabb box{Vec4::Splat(constants::kMaxFloat.x),
                         Vec4::Splat(-constants::kMaxFloat.x)};
                for (const Vec4& v : s.hull.vertices) {
                    const Vec4 p = ToWorld(xf, v);
                    box.lower = {std::min(box.lower.x, p.x), std::min(box.lower.y, p.y),
                                 std::min(box.lower.z, p.z)};
                    box.upper = {std::max(box.upper.x, p.x), std::max(box.upper.y, p.y),
                                 std::max(box.upper.z, p.z)};
                }
                const Vec4 skin = Vec4::Splat(margin);
                return Aabb{box.lower - skin, box.upper + skin};
            }
        },
        shape);
}

void ShapeExtents(const Shape& shape, f32& minExtent, f32& maxExtent) {
    std::visit(
        [&](const auto& s) {
            using T = std::decay_t<decltype(s)>;
            if constexpr (std::is_same_v<T, Sphere>) {
                // The max is the *centre's* distance from the body origin, not centre plus
                // radius: rotation sweeps the sphere's centre and the sphere itself is
                // rotation-invariant, so a centred sphere spinning in place is never fast.
                minExtent = s.radius;
                maxExtent = std::sqrt(LengthSquared3(s.centre));
            } else if constexpr (std::is_same_v<T, Capsule>) {
                minExtent = s.radius;
                maxExtent = std::max(std::sqrt(LengthSquared3(s.p1)),
                                     std::sqrt(LengthSquared3(s.p2)));
            } else if constexpr (std::is_same_v<T, Polytope>) {
                // The nearest face plane from the centroid, and the farthest vertex from the
                // body origin. A degenerate hull reports {FLT_MAX, 0}, which reads as "never
                // fast".
                minExtent = constants::kMaxFloat.x;
                for (const HullFace& face : s.hull.faces) {
                    minExtent =
                        std::min(minExtent, face.offset - Dot3(face.normal, s.centroid));
                }
                f32 far = 0.0f;
                for (const Vec4& v : s.hull.vertices) {
                    far = std::max(far, LengthSquared3(v));
                }
                maxExtent = std::sqrt(far);
            } else {
                // Triangles and meshes are never dynamic fixtures; their extents stay at the
                // zero-fill, which also reads as "never fast".
                minExtent = 0.0f;
                maxExtent = 0.0f;
            }
        },
        shape);
}

MassProperties ComputeMass(const Shape& shape, f32 density) {
    return std::visit(
        [density](const auto& s) {
            using T = std::decay_t<decltype(s)>;
            if constexpr (std::is_same_v<T, Sphere> || std::is_same_v<T, Polytope> ||
                          std::is_same_v<T, Capsule>) {
                return ComputeMass(s, density);
            } else {
                // Triangles and meshes are static geometry: a triangle has no volume to
                // weigh, and a fixture built on either is static by construction.
                return MassProperties{};
            }
        },
        shape);
}

}  // namespace snowball
