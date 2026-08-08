#pragma once

#include <cornflakes/interface/core/arena.hpp>
#include <cornflakes/interface/core/types.hpp>
#include <cornflakes/interface/diagnostics/issue.hpp>

#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <string_view>

namespace whiteout::cornflakes {

inline constexpr u8 kPkmmFileVersion = 0U;

enum class MeshScalarType : u8 {
    U8,
    I8,
    U16,
    I16,
    U32,
    I32,
    F16,
    F32,
};

constexpr u32 meshScalarSize(MeshScalarType t) noexcept {
    switch (t) {
    case MeshScalarType::U8:
    case MeshScalarType::I8:
        return 1U;
    case MeshScalarType::U16:
    case MeshScalarType::I16:
    case MeshScalarType::F16:
        return 2U;
    default:
        return 4U;
    }
}

enum class MeshIndexFormat : u8 {
    U8,
    U16,
    U32,
};

constexpr u32 meshIndexSize(MeshIndexFormat f) noexcept {
    return f == MeshIndexFormat::U8 ? 1U : (f == MeshIndexFormat::U16 ? 2U : 4U);
}

enum class MeshPrimitive : u8 {
    TriangleStrips,
    Triangles,
    Lines,
};

struct MeshVertexStream {
    std::string_view semantic;
    MeshScalarType scalar = MeshScalarType::F32;
    u8 components = 1U;
    bool normalized = false;
    bool stride16 = false;
    std::span<const std::byte> data;

    u32 footprint() const noexcept {
        const u32 natural = meshScalarSize(scalar) * components;
        return (stride16 && natural == 12U) ? 16U : natural;
    }
};

struct MeshPdfSlot {
    f32 proba = 1.0F;
    u32 other = 0U;
};

struct MeshPdf {
    std::span<const MeshPdfSlot> slots;
    f32 totalDensity = 0.0F;

    bool empty() const noexcept {
        return slots.empty();
    }
};

struct MeshKdNode {
    u32 nodeData = 0U;
    f32 splitPos = 0.0F;

    bool isLeaf() const noexcept { return (nodeData & 1U) != 0U; }
    u32 axis() const noexcept { return (nodeData & 6U) >> 1U; }
    u32 leafElement() const noexcept { return nodeData >> 2U; }
    u32 secondChildOffsetNodes() const noexcept { return (nodeData & ~7U) / 8U; }
};

struct MeshKdTree {
    std::array<f32, 3> bboxMin{};
    std::array<f32, 3> bboxMax{};
    std::span<const MeshKdNode> nodes;
    std::span<const u32> leaves;
    u8 compressionType = 0U;
    bool framedByCompressor = false;

    bool valid() const noexcept { return !nodes.empty() && !leaves.empty(); }
};

struct MeshWeightedVertex {
    std::array<u32, 3> indices{};
    std::array<f32, 2> weights{};
};

struct MeshTetrahedra {
    std::span<const u32> indices;
    std::span<const MeshWeightedVertex> weighted;
    u32 flags = 0U;

    u32 count() const noexcept {
        return static_cast<u32>(indices.size() / 4U);
    }
    bool valid() const noexcept {
        return !indices.empty() && indices.size() % 4U == 0U;
    }
};

struct MeshGeometry {
    std::string_view name;
    std::string_view material;
    MeshPrimitive primitive = MeshPrimitive::Triangles;
    MeshIndexFormat indexFormat = MeshIndexFormat::U16;
    u32 indexCount = 0U;
    std::span<const std::byte> indices;
    u32 vertexCount = 0U;
    std::span<const MeshVertexStream> streams;

    bool hasSurfaceVolume = false;
    f32 surface = 0.0F;
    f32 volume = 0.0F;
    bool hasBBox = false;
    std::array<f32, 3> bboxMin{};
    std::array<f32, 3> bboxMax{};

    MeshPdf surfaceSamplingPdf;
    MeshPdf volumeSamplingPdf;
    MeshTetrahedra tetrahedra;
    MeshKdTree kdTree;

    bool deltaEncoded = false;
    bool simd = false;
    bool quantized = false;
    std::array<u8, 3> quantBits{};
    std::array<f32, 3> quantExtents{};

    u32 index(u32 i) const noexcept;

    u32 triangleCount() const noexcept {
        return primitive == MeshPrimitive::Triangles ? indexCount / 3U : 0U;
    }

    const MeshVertexStream* stream(std::string_view semantic) const noexcept;
};

struct MeshAsset {
    u8 version = kPkmmFileVersion;
    std::span<const MeshGeometry> geometries;
    bool hasSkeleton = false;
    std::optional<u8> coordinateFrame;
    std::array<u8, 3> coordinateFrameAxes{};

    std::array<u8, 3> frameAxes() const noexcept;
};

inline constexpr u8 kEngineCoordinateFrame = 1U;

std::array<u8, 3> meshFrameAxes(u8 frame) noexcept;

struct MeshAxisMap {
    u8 source = 0U;
    f32 sign = 1.0F;
};

std::array<MeshAxisMap, 3> meshTransposeMap(std::array<u8, 3> from, std::array<u8, 3> to) noexcept;

std::array<f32, 3> meshTranspose(const std::array<MeshAxisMap, 3>& map,
                                 std::array<f32, 3> v) noexcept;

bool meshTransposeMirrors(const std::array<MeshAxisMap, 3>& map) noexcept;

std::optional<MeshAsset> readPkmm(std::span<const std::byte> bytes, IArena& arena,
                                  IssueBag& issues);

}
