#pragma once

#include <cornflakes/interface/asset/vector_field_asset.hpp>
#include <cornflakes/interface/core/types.hpp>

#include <array>
#include <span>
#include <string_view>

namespace whiteout::cornflakes {

struct MeshShapeData;

struct TextureImageData;

enum class SamplerKind : u8 {
    Unknown = 0,
    Curve,
    Shape,
    EventStream,
    Turbulence,
    Texture,
};

struct SamplerCurve {
    std::span<const f32> times;
    std::span<const f32> values;
    std::span<const f32> tangents;
    u8 components = 1;
    u32 interpolator = 0;
    bool looped = false;

    bool isProbabilityCurve = false;

    std::span<const f32> cdfTimes;
    std::span<const f32> cdfValues;
};

enum class ShapeType : u32 {

    Box = 0,
    Sphere = 1,
    ComplexEllipsoid = 2,
    Cylinder = 3,
    Capsule = 4,
    Cone = 5,
    Mesh = 6,
};

enum class SampleDimensionality : u32 {
    Vertex = 1,
    Surface = 2,
    Volume = 3,
};

struct SamplerShape {
    ShapeType type = ShapeType::Box;
    SampleDimensionality dimensionality = SampleDimensionality::Surface;
    std::array<f32, 3> boxDimensions{0.5F, 0.5F, 0.5F};
    f32 radius = 0.0F;
    f32 innerRadius = 0.0F;
    f32 height = 0.0F;
    bool hemisphere = false;

    std::array<f32, 3> position{0.0F, 0.0F, 0.0F};
    std::array<f32, 3> eulerOrientation{0.0F, 0.0F,
                                        0.0F};
    std::array<f32, 3> nonUniformScale{1.0F, 1.0F, 1.0F};
    bool transformTranslate = false;
    bool transformRotate = false;

    std::string_view meshResource;
    std::array<f32, 3> meshScale{1.0F, 1.0F, 1.0F};
    u32 meshSamplingMode = 1U;
    u32 subMeshIndex = 0U;
    u32 defaultUvStream = 0U;
    u32 defaultColorStream = 0U;
    u32 densityColorStream = 0U;
    u32 densityChannel = 0U;
    bool meshHasTetrahedra = false;
    const MeshShapeData* mesh = nullptr;
};

struct SamplerEventStream {
    std::span<const f32> times;
};

enum class TurbulenceDataSource : u32 {
    Procedural = 0,
    External = 1,
};

enum class VectorFieldInterpolation : u32 {
    Point = 0,
    Trilinear = 1,
    Quadrilinear = 2,
};

struct SamplerVectorField {
    std::string_view resource;
    std::span<const std::byte> data;
    std::array<u32, 4> dimensions{};
    std::array<f32, 3> gridScale{};
    std::array<f32, 3> gridOffset{};
    std::array<u32, 3> strideL2{};
    std::array<i32, 3> addrMask{};
    f32 timeScale = 0.0F;
    bool wrapTime = true;
    f32 vecScale = 0.0F;
    VectorFieldDataType dataType = VectorFieldDataType::Fp32;
    VectorFieldInterpolation interpolation = VectorFieldInterpolation::Trilinear;
    bool valid = false;
};

struct SamplerTurbulence {
    TurbulenceDataSource dataSource = TurbulenceDataSource::Procedural;
    SamplerVectorField vectorField;

    f32 strength = 0.1F;
    f32 wavelength = 0.5F;
    f32 globalScale = 1.0F;
    f32 lacunarity = 0.5F;
    f32 gain = 0.5F;
    f32 gainMultiplier = 1.0F;
    u32 octaves = 2U;
    u32 interpolator = 1U;
    u32 seed = 1114229502U;
    f32 timeScale = 0.0F;
    f32 timeBase = 0.0F;
    f32 timeRandomVariation = 0.5F;
    f32 delta = 1.0e-4F;
    f32 dnorm = 5000.0F;
    std::span<const f32> gradients;
    std::span<const f32> rigidBasis;
    std::span<const f32> spinRate;
};

enum class TextureGammaSpace : u32 {
    Linear = 0,
    SRGB = 1,
    LinearToSRGB = 2,
    SRGBToLinear = 3,
};

enum class TextureDensitySrc : u32 {
    Red = 0,
    Green = 1,
    Blue = 2,
    Alpha = 3,
    RgbaAverage = 4,
};

struct SamplerTexture {
    std::string_view textureResource;
    std::string_view atlasDefinition;
    u32 scriptOutputType = 4U;
    bool sampleRawValues = true;
    TextureGammaSpace gammaSpace = TextureGammaSpace::Linear;

    f32 densityPower = 1.0F;
    TextureDensitySrc densitySrc = TextureDensitySrc::RgbaAverage;
    std::array<f32, 4> densityRgbaWeights{0.212671F, 0.71516F, 0.072169F, 0.0F};

    const TextureImageData* image = nullptr;
};

struct SamplerResource {
    std::string_view name;
    SamplerKind kind = SamplerKind::Unknown;

    SamplerCurve curve;
    SamplerShape shape;
    SamplerEventStream eventStream;
    SamplerTurbulence turbulence;
    SamplerTexture texture;
};

f32 evalSamplerCurveScalar(const SamplerCurve& curve, f32 t, f32 defaultValue = 0.0F) noexcept;

u8 evalSamplerCurveVec(const SamplerCurve& curve, f32 t, f32* out, u8 outLen) noexcept;

f32 evalSamplerCurveCdf(const SamplerCurve& curve, f32 t, f32 defaultValue = 0.0F) noexcept;

const SamplerResource* findSamplerByName(std::span<const SamplerResource> samplers,
                                         std::string_view name) noexcept;

u32 samplerBindGeneration() noexcept;

void bumpSamplerBindGeneration() noexcept;

}
