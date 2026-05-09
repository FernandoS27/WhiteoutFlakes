#pragma once

/// @file
/// @brief Bound sampler resources (curves, shapes, event streams) attached to a layer.

#include <cornflakes/interface/core/types.hpp>

#include <array>
#include <span>
#include <string_view>

namespace whiteout::cornflakes {

/// @brief Discriminator selecting which payload of `SamplerResource` is live.
enum class SamplerKind : u8 {
    Unknown = 0,
    Curve,
    Shape,
    EventStream,
};

/// @brief Tabulated curve sampler — times, values, optional Hermite tangents.
struct SamplerCurve {
    std::span<const f32> times;
    std::span<const f32> values;
    std::span<const f32> tangents;
    u8 components = 1;       ///< Channel count (1..4); set from asset, not derived from spans.
    u32 interpolator = 0;
    bool looped = false;
};

/// @brief Shape kind for `SamplerShape`. Values match the asset enum.
enum class ShapeType : u32 {

    Box = 0,
    Sphere = 1,
    Cone = 2,
    Cylinder = 3,
    Mesh = 4,
};

/// @brief Spatial shape sampler (cone/box/sphere/cylinder/mesh) with TRS transform.
///
/// `eulerOrientation` is stored in DEGREES (matches CornFx asset format);
/// the binder converts to radians before constructing the orientation quat.
struct SamplerShape {
    ShapeType type = ShapeType::Box;
    std::array<f32, 3> boxDimensions{};
    f32 radius = 0.0F;
    f32 innerRadius = 0.0F;
    f32 height = 0.0F;
    bool hemisphere = false;

    std::array<f32, 3> position{0.0F, 0.0F, 0.0F};
    std::array<f32, 3> eulerOrientation{0.0F, 0.0F, 0.0F}; ///< Degrees; converted to radians at bind.
    std::array<f32, 3> nonUniformScale{1.0F, 1.0F, 1.0F};
    bool transformTranslate = false;
    bool transformRotate = false;
};

/// @brief Discrete event-time list used by EventStream samplers.
struct SamplerEventStream {
    std::span<const f32> times;
};

/// @brief One named sampler bound to a layer — the active payload is selected by `kind`.
struct SamplerResource {
    std::string_view name;
    SamplerKind kind = SamplerKind::Unknown;

    SamplerCurve curve;
    SamplerShape shape;
    SamplerEventStream eventStream;
};

/// @brief Sample channel 0 of `curve` at `t`; returns `defaultValue` when empty.
f32 evalSamplerCurveScalar(const SamplerCurve& curve, f32 t, f32 defaultValue = 0.0F) noexcept;

/// @brief Sample up to `outLen` channels of `curve` at `t` into `out`.
/// @return Number of channels written (≤ `outLen`).
u8 evalSamplerCurveVec(const SamplerCurve& curve, f32 t, f32* out, u8 outLen) noexcept;

const SamplerResource* findSamplerByName(std::span<const SamplerResource> samplers,
                                         std::string_view name) noexcept;

} // namespace whiteout::cornflakes
