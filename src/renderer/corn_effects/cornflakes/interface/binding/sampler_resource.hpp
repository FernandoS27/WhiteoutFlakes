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
    Turbulence,
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
/// `eulerOrientation` is stored in DEGREES;
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

/// @brief Procedural fractal curl-noise turbulence sampler (engine-faithful).
///
/// Mirrors `CParticleSamplerDescriptor_Turbulence_Default::Sample @ preview.exe
/// 0x7ff60a46c5f0`: value-noise base (`CoherentSample*`) hashed through the engine
/// permutation table, MT-seeded gradients, fBm octaves (`_SetupNoiseSampler @
/// 0x7ff60a4f3b60`), and a finite-difference curl (FastFakeFlow path, δ=1e-4,
/// DNorm=5000). `gradients` is precomputed at bind time (256 values); empty → zero field.
///
/// **Engine-faithful, NOT yet bit-exact** — see `TURBULENCE_RE_FINDINGS.md` for the
/// documented gaps (exact MT19937 variant, true-flow analytic curl, RotateBasis time
/// animation, coordinate remap, several HBO field defaults).
struct SamplerTurbulence {
    f32 strength = 1.0F;       ///< Output amplitude.
    f32 wavelength = 1.0F;     ///< Base spatial scale (large = coarse features).
    f32 globalScale = 1.0F;
    f32 lacunarity = 2.0F;     ///< Per-octave frequency multiplier (≥1 → coarser octaves).
    f32 gain = 0.5F;           ///< Per-octave amplitude multiplier.
    f32 gainMultiplier = 1.0F; ///< Engine default 1.0 (RE-confirmed).
    u32 octaves = 1U;          ///< fBm octave count (clamped to 24).
    u32 interpolator = 0U;     ///< 0=Linear, 1=Quintic, 2=Cubic.
    u32 seed = 0U;             ///< InitialSeed → MT gradient generation.
    f32 timeScale = 0.0F;      ///< Animation: rotates the field by time*timeScale+timeBase.
    f32 timeBase = 0.0F;
    f32 delta = 1.0e-4F;       ///< FD-curl step (`m_Delta`).
    f32 dnorm = 5000.0F;       ///< FD-curl normalizer `1/(2·delta)` (`m_DNorm`).
    std::span<const f32> gradients; ///< 256 MT-seeded value-noise gradients; empty → zero field.
};

/// @brief One named sampler bound to a layer — the active payload is selected by `kind`.
struct SamplerResource {
    std::string_view name;
    SamplerKind kind = SamplerKind::Unknown;

    SamplerCurve curve;
    SamplerShape shape;
    SamplerEventStream eventStream;
    SamplerTurbulence turbulence;
};

/// @brief Sample channel 0 of `curve` at `t`; returns `defaultValue` when empty.
f32 evalSamplerCurveScalar(const SamplerCurve& curve, f32 t, f32 defaultValue = 0.0F) noexcept;

/// @brief Sample up to `outLen` channels of `curve` at `t` into `out`.
/// @return Number of channels written (≤ `outLen`).
u8 evalSamplerCurveVec(const SamplerCurve& curve, f32 t, f32* out, u8 outLen) noexcept;

const SamplerResource* findSamplerByName(std::span<const SamplerResource> samplers,
                                         std::string_view name) noexcept;

} // namespace whiteout::cornflakes
