#include <cornflakes/core/determinism.hpp>
#include <cornflakes/sampler/shape.hpp>

#include <cmath>

namespace whiteout::cornflakes {

namespace {

constexpr f32 kPi = 3.14159265358979323846F;
constexpr f32 kTwoPi = 2.0F * kPi;

enum class BoxFace : i32 {
    PlusX = 0,
    MinusX = 1,
    PlusY = 2,
    MinusY = 3,
    PlusZ = 4,
    MinusZ = 5,
    Count,
};

ShapeSampleResult sampleSphere(const ShapeSphere& s, f32 u, f32 v) noexcept {
    const f32 azimuth = u * kTwoPi;
    const f32 polar = v * kPi;
    const f32 sinPolar = std::sin(polar);

    const Float3 direction{
        sinPolar * std::cos(azimuth),
        sinPolar * std::sin(azimuth),
        std::cos(polar),
    };

    ShapeSampleResult out;
    out.position = {
        s.center.x + direction.x * s.radius,
        s.center.y + direction.y * s.radius,
        s.center.z + direction.z * s.radius,
    };
    out.normal = direction;
    return out;
}

ShapeSampleResult sampleBox(const ShapeBox& b, f32 u, f32 v) noexcept {
    constexpr i32 kFaceCount = static_cast<i32>(BoxFace::Count);
    const f32 scaledU = u * static_cast<f32>(kFaceCount);
    i32 faceIdx = static_cast<i32>(std::floor(scaledU));
    if (faceIdx < 0) {
        faceIdx = static_cast<i32>(BoxFace::PlusX);
    }
    if (faceIdx >= kFaceCount) {
        faceIdx = kFaceCount - 1;
    }
    const auto face = static_cast<BoxFace>(faceIdx);
    const f32 faceU = scaledU - static_cast<f32>(faceIdx);

    const f32 a = (2.0F * faceU) - 1.0F;
    const f32 b2 = (2.0F * v) - 1.0F;

    ShapeSampleResult out;
    out.position = b.center;

    switch (face) {
    case BoxFace::PlusX:
        out.position.x += b.halfExtents.x;
        out.position.y += a * b.halfExtents.y;
        out.position.z += b2 * b.halfExtents.z;
        out.normal = {1.0F, 0.0F, 0.0F};
        break;
    case BoxFace::MinusX:
        out.position.x -= b.halfExtents.x;
        out.position.y += a * b.halfExtents.y;
        out.position.z += b2 * b.halfExtents.z;
        out.normal = {-1.0F, 0.0F, 0.0F};
        break;
    case BoxFace::PlusY:
        out.position.x += a * b.halfExtents.x;
        out.position.y += b.halfExtents.y;
        out.position.z += b2 * b.halfExtents.z;
        out.normal = {0.0F, 1.0F, 0.0F};
        break;
    case BoxFace::MinusY:
        out.position.x += a * b.halfExtents.x;
        out.position.y -= b.halfExtents.y;
        out.position.z += b2 * b.halfExtents.z;
        out.normal = {0.0F, -1.0F, 0.0F};
        break;
    case BoxFace::PlusZ:
        out.position.x += a * b.halfExtents.x;
        out.position.y += b2 * b.halfExtents.y;
        out.position.z += b.halfExtents.z;
        out.normal = {0.0F, 0.0F, 1.0F};
        break;
    case BoxFace::MinusZ:
    case BoxFace::Count:
    default:
        out.position.x += a * b.halfExtents.x;
        out.position.y += b2 * b.halfExtents.y;
        out.position.z -= b.halfExtents.z;
        out.normal = {0.0F, 0.0F, -1.0F};
        break;
    }
    return out;
}

} // namespace

ShapeSampleResult sampleShapeSurface(const ShapeSampler& s, f32 u, f32 v) noexcept {
    switch (s.kind) {
    case ShapeKind::Sphere:
        return sampleSphere(s.sphere, u, v);
    case ShapeKind::Box:
        return sampleBox(s.box, u, v);
    }
    return {};
}

} // namespace whiteout::cornflakes
