#pragma once

#include <cornflakes/interface/asset/mesh_asset.hpp>
#include <cornflakes/interface/core/types.hpp>

#include <array>
#include <span>

namespace whiteout::cornflakes {

enum class MeshSamplingDistribution : u32 {
    Fast = 0U,
    Uniform = 1U,
    Weighted = 2U,
};

struct MeshAliasSlot {
    f32 proba = 1.0F;
    u32 other = 0U;
};

struct MeshBvhNode {
    std::array<f32, 3> boundsMin{};
    std::array<f32, 3> boundsMax{};
    u32 firstTriangle = 0U;
    u32 triangleCount = 0U;
    u32 leftChild = 0U;
    u32 rightChild = 0U;
};

struct MeshAxisRemap {
    u8 source = 0U;
    f32 sign = 1.0F;
};

struct alignas(16) MeshProjTriangle {
    std::array<f32, 4> vx{};
    std::array<f32, 4> vy{};
    std::array<f32, 4> vz{};
    std::array<f32, 4> nx{};
    std::array<f32, 4> ny{};
    std::array<f32, 4> nz{};
    std::array<f32, 4> norm{};
    std::array<f32, 4> bari{};
};

struct MeshProjConfig {
    f32 delta = 0.0F;
    f32 deltaStep = 0.0F;
    f32 deltaGrow = 1.0F;
    u32 maxIter = 0U;
};

struct MeshShapeData {
    std::span<const f32> positions;
    std::span<const f32> normals;
    std::span<const f32> tangents;
    std::span<const std::span<const f32>> texcoordStreams;
    std::span<const std::span<const f32>> colorStreams;
    u32 defaultUvStream = 0U;
    u32 defaultColorStream = 0U;
    std::span<const u32> indices;
    u32 vertexCount = 0U;
    u32 triangleCount = 0U;
    std::span<const MeshAliasSlot> alias;
    bool aliasFromAsset = false;
    f32 surfaceArea = 0.0F;
    f32 volume = 0.0F;
    std::span<const MeshBvhNode> bvh;
    std::span<const u32> bvhTriangles;

    std::span<const MeshKdNode> kdNodes;
    std::span<const u32> kdLeaves;
    std::array<f32, 3> kdBboxMin{};
    std::array<f32, 3> kdBboxMax{};
    std::span<const MeshProjTriangle> projTriangles;
    MeshProjConfig projConfig;
    std::array<MeshAxisRemap, 3> engineToFile{};
    bool projWindingFlipped = false;

    bool hasEngineProjection() const noexcept {
        return !kdNodes.empty() && !kdLeaves.empty() && !projTriangles.empty() &&
               projConfig.maxIter != 0U;
    }

    std::span<const f32> texcoords(i32 n = -1) const noexcept {
        const u32 idx = n < 0 ? defaultUvStream : static_cast<u32>(n);
        return idx < texcoordStreams.size() ? texcoordStreams[idx] : std::span<const f32>{};
    }
    std::span<const f32> colors(i32 n = -1) const noexcept {
        const u32 idx = n < 0 ? defaultColorStream : static_cast<u32>(n);
        return idx < colorStreams.size() ? colorStreams[idx] : std::span<const f32>{};
    }
};

struct MeshParametricCoords {
    u32 triangle = 0U;
    f32 u = 0.0F;
    f32 v = 0.0F;
};

struct MeshSurfaceSample {
    std::array<f32, 3> position{};
    std::array<f32, 3> normal{};
    std::array<f32, 2> texcoord{};
    std::array<f32, 4> color{};
    u32 triangle = 0U;
    f32 u = 0.0F;
    f32 v = 0.0F;
};

enum class MeshField : u32 {
    Position = 0U,
    Normal = 1U,
    Tangent = 2U,
    Texcoord = 3U,
    Color = 4U,
    Velocity = 5U,
};

u8 sampleMeshField(const MeshShapeData& mesh, MeshField field, i32 streamIndex,
                   const MeshParametricCoords& pc, f32* out) noexcept;

constexpr u8 meshFieldComponents(MeshField field, i32 streamIndex) noexcept {
    switch (field) {
    case MeshField::Tangent:
        return 4U;
    case MeshField::Texcoord:
        return 2U;
    case MeshField::Color:
        return streamIndex < 0 ? 4U : 2U;
    default:
        return 3U;
    }
}

#if defined(CORNFLAKES_MESH_PROBE)
void reportMeshProbe() noexcept;
#endif

bool projectMeshPointEngine(const MeshShapeData& mesh, const std::array<f32, 3>& point,
                            MeshParametricCoords& out) noexcept;

void buildMeshProjTriangles(std::span<const f32> positions, std::span<const u32> indices,
                            std::span<MeshProjTriangle> out) noexcept;

MeshProjConfig meshProjConfigFor(const std::array<f32, 3>& bboxMin,
                                 const std::array<f32, 3>& bboxMax) noexcept;

bool projectMeshPoint(const MeshShapeData& mesh, const std::array<f32, 3>& point,
                      MeshParametricCoords& out) noexcept;

u32 buildMeshBvh(const MeshShapeData& mesh, std::span<MeshBvhNode> nodes,
                 std::span<u32> triangleOrder) noexcept;

constexpr u32 meshBvhNodeBudget(u32 triangleCount) noexcept {
    return triangleCount == 0U ? 0U : 2U * triangleCount + 1U;
}

f32 meshTriangleAreaCoeff(const MeshShapeData& mesh, u32 triangle) noexcept;

void buildMeshTriangleDensities(const MeshShapeData& mesh, std::span<const f32> vertexDensity,
                                std::span<f32> out) noexcept;

bool buildMeshAliasTable(std::span<f32> densities, std::span<MeshAliasSlot> out) noexcept;

u32 sampleMeshTriangle(const MeshShapeData& mesh, MeshSamplingDistribution mode, f32 r0,
                       f32 r3) noexcept;

MeshSurfaceSample sampleMeshSurface(const MeshShapeData& mesh, MeshSamplingDistribution mode,
                                    f32 r0, f32 r1, f32 r2, f32 r3) noexcept;

constexpr u32 meshSampleRandomCount(MeshSamplingDistribution mode) noexcept {
    return mode == MeshSamplingDistribution::Fast ? 3U : 4U;
}

}
