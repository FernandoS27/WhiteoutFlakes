
#include <atomic>
#include <cstdio>
#include <cornflakes/sampler/mesh.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>

#if defined(_M_X64) || defined(_M_AMD64) || defined(__SSE__)
#include <immintrin.h>
#endif
#include <limits>
#include <vector>

namespace whiteout::cornflakes {

namespace {

u32 truncIndex(f32 v, u32 count) noexcept {
    if (!(v > 0.0F) || count == 0U) {
        return 0U;
    }
    const auto i = static_cast<u32>(v);
    return i < count ? i : count - 1U;
}

std::array<f32, 3> vertexPos(const MeshShapeData& mesh, u32 vi) noexcept {
    const std::size_t o = static_cast<std::size_t>(vi) * 3U;
    if (o + 3U > mesh.positions.size()) {
        return {};
    }
    return {mesh.positions[o], mesh.positions[o + 1U], mesh.positions[o + 2U]};
}

std::array<u32, 3> triangleIndices(const MeshShapeData& mesh, u32 triangle) noexcept {
    const std::size_t o = static_cast<std::size_t>(triangle) * 3U;
    if (o + 3U > mesh.indices.size()) {
        return {};
    }
    return {mesh.indices[o], mesh.indices[o + 1U], mesh.indices[o + 2U]};
}

constexpr f32 kInf = std::numeric_limits<f32>::infinity();

void interpolateStream(const MeshShapeData& mesh, std::span<const f32> stream, u32 comps,
                       const MeshParametricCoords& pc, f32* dst) noexcept {
    if (stream.empty()) {
        return;
    }
    const auto idx = triangleIndices(mesh, pc.triangle);
    const f32 weights[3] = {pc.u, pc.v, 1.0F - pc.u - pc.v};
    for (u32 c = 0U; c < comps; ++c) {
        f32 acc = 0.0F;
        for (u32 k = 0U; k < 3U; ++k) {
            const std::size_t o = static_cast<std::size_t>(idx[k]) * comps + c;
            if (o < stream.size()) {
                acc += stream[o] * weights[k];
            }
        }
        dst[c] = acc;
    }
}

f32 boxDistanceSq(const MeshBvhNode& n, const std::array<f32, 3>& p) noexcept {
    f32 d = 0.0F;
    for (u32 c = 0U; c < 3U; ++c) {
        const f32 v = p[c] < n.boundsMin[c]
                          ? n.boundsMin[c] - p[c]
                          : (p[c] > n.boundsMax[c] ? p[c] - n.boundsMax[c] : 0.0F);
        d += v * v;
    }
    return d;
}

f32 closestOnTriangle(const MeshShapeData& mesh, u32 t, const std::array<f32, 3>& p, f32& outU,
                      f32& outV) noexcept {
    const auto idx = triangleIndices(mesh, t);
    const auto a = vertexPos(mesh, idx[0]);
    const auto b = vertexPos(mesh, idx[1]);
    const auto c = vertexPos(mesh, idx[2]);
    const auto sub = [](const std::array<f32, 3>& x, const std::array<f32, 3>& y) {
        return std::array<f32, 3>{x[0] - y[0], x[1] - y[1], x[2] - y[2]};
    };
    const auto dot = [](const std::array<f32, 3>& x, const std::array<f32, 3>& y) {
        return x[0] * y[0] + x[1] * y[1] + x[2] * y[2];
    };
    const auto ab = sub(b, a);
    const auto ac = sub(c, a);
    const auto ap = sub(p, a);

    const f32 d1 = dot(ab, ap);
    const f32 d2 = dot(ac, ap);
    f32 wb = 0.0F;
    f32 wc = 0.0F;
    if (d1 > 0.0F || d2 > 0.0F) {
        const auto bp = sub(p, b);
        const f32 d3 = dot(ab, bp);
        const f32 d4 = dot(ac, bp);
        const auto cp = sub(p, c);
        const f32 d5 = dot(ab, cp);
        const f32 d6 = dot(ac, cp);
        const f32 vc = d1 * d4 - d3 * d2;
        const f32 vb = d5 * d2 - d1 * d6;
        const f32 va = d3 * d6 - d5 * d4;
        if (d3 >= 0.0F && d4 <= d3) {
            wb = 1.0F;
        } else if (d6 >= 0.0F && d5 <= d6) {
            wc = 1.0F;
        } else if (vc <= 0.0F && d1 >= 0.0F && d3 <= 0.0F) {
            wb = d1 / (d1 - d3);
        } else if (vb <= 0.0F && d2 >= 0.0F && d6 <= 0.0F) {
            wc = d2 / (d2 - d6);
        } else if (va <= 0.0F && (d4 - d3) >= 0.0F && (d5 - d6) >= 0.0F) {
            const f32 den = (d4 - d3) + (d5 - d6);
            const f32 k = den > 0.0F ? (d4 - d3) / den : 0.0F;
            wb = 1.0F - k;
            wc = k;
        } else {
            const f32 den = va + vb + vc;
            if (den > 0.0F) {
                wb = vb / den;
                wc = vc / den;
            }
        }
    }
    const f32 wa = 1.0F - wb - wc;
    f32 distSq = 0.0F;
    for (u32 k = 0U; k < 3U; ++k) {
        const f32 q = a[k] * wa + b[k] * wb + c[k] * wc;
        const f32 d = p[k] - q;
        distSq += d * d;
    }
    outU = wa;
    outV = wb;
    return distSq;
}

}

f32 meshTriangleAreaCoeff(const MeshShapeData& mesh, u32 triangle) noexcept {
    const auto idx = triangleIndices(mesh, triangle);
    const auto a = vertexPos(mesh, idx[0]);
    const auto b = vertexPos(mesh, idx[1]);
    const auto c = vertexPos(mesh, idx[2]);
    const f32 e0x = b[0] - a[0];
    const f32 e0y = b[1] - a[1];
    const f32 e0z = b[2] - a[2];
    const f32 e1x = c[0] - a[0];
    const f32 e1y = c[1] - a[1];
    const f32 e1z = c[2] - a[2];
    const f32 cx = e0y * e1z - e0z * e1y;
    const f32 cy = e0z * e1x - e0x * e1z;
    const f32 cz = e0x * e1y - e0y * e1x;
    return std::sqrt(cx * cx + cy * cy + cz * cz);
}

void buildMeshTriangleDensities(const MeshShapeData& mesh, std::span<const f32> vertexDensity,
                                std::span<f32> out) noexcept {
    const u32 n = static_cast<u32>(std::min<std::size_t>(out.size(), mesh.triangleCount));
    for (u32 t = 0U; t < n; ++t) {
        f32 w = 1.0F;
        if (!vertexDensity.empty()) {
            const auto idx = triangleIndices(mesh, t);
            f32 sum = 0.0F;
            for (const u32 vi : idx) {
                if (vi < vertexDensity.size()) {
                    sum += vertexDensity[vi];
                }
            }
            w = sum > 0.0F ? sum : 0.0F;
        }
        out[t] = w * meshTriangleAreaCoeff(mesh, t);
    }
}

bool buildMeshAliasTable(std::span<f32> densities, std::span<MeshAliasSlot> out) noexcept {
    const std::size_t count = std::min(densities.size(), out.size());
    if (count == 0U) {
        return false;
    }

    f32 totalDensity = 0.0F;
    for (std::size_t i = 0; i < count; ++i) {
        if (!(densities[i] >= 0.0F) || !std::isfinite(densities[i])) {
            densities[i] = 0.0F;
        }
        totalDensity += densities[i];
    }
    if (!std::isfinite(totalDensity) || totalDensity <= 0.0F) {
        return false;
    }

    const f32 densityPerSlot = totalDensity / static_cast<f32>(count);
    const f32 rcpDensityPerSlot = 1.0F / densityPerSlot;

    constexpr u32 kInvalid = 0xFFFFFFFFU;
    for (std::size_t i = 0; i < count; ++i) {
        out[i].other = kInvalid;
    }

    std::vector<u32> smalls;
    std::vector<u32> bigs;
    smalls.reserve(count);
    bigs.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        if (densityPerSlot < densities[i]) {
            bigs.push_back(static_cast<u32>(i));
        } else {
            smalls.push_back(static_cast<u32>(i));
        }
    }

    if (!smalls.empty() && !bigs.empty()) {
        u32 bigSlotId = bigs.back();
        f32 big = densities[bigSlotId];
        while (!smalls.empty()) {
            const u32 smallSlotId = smalls.back();
            smalls.pop_back();
            const f32 small = densities[smallSlotId];
            const f32 neededDensity = densityPerSlot - small;
            densities[smallSlotId] = 0.0F;

            out[smallSlotId].other = bigSlotId;
            out[smallSlotId].proba = small * rcpDensityPerSlot;

            big -= neededDensity;
            if (densityPerSlot >= big) {
                smalls.push_back(bigSlotId);
                densities[bigSlotId] = big;
                bigs.pop_back();
                if (bigs.empty()) {
                    break;
                }
                bigSlotId = bigs.back();
                big = densities[bigSlotId];
            }
        }
        densities[bigSlotId] = big;
    }

    for (const u32 i : bigs) {
        if (out[i].other == kInvalid) {
            out[i].proba = 1.0F;
            out[i].other = i;
        }
    }
    for (const u32 i : smalls) {
        if (out[i].other == kInvalid) {
            out[i].proba = 1.0F;
            out[i].other = i;
        }
    }
    for (std::size_t i = 0; i < count; ++i) {
        if (out[i].other == kInvalid) {
            out[i].proba = 1.0F;
            out[i].other = static_cast<u32>(i);
        }
    }
    return true;
}

u32 sampleMeshTriangle(const MeshShapeData& mesh, MeshSamplingDistribution mode, f32 r0,
                       f32 r3) noexcept {
    if (mesh.triangleCount == 0U) {
        return 0U;
    }
    if (mode == MeshSamplingDistribution::Fast || mesh.alias.size() < mesh.triangleCount) {
        return truncIndex(r0 * static_cast<f32>(mesh.triangleCount), mesh.triangleCount);
    }
    const u32 index = truncIndex(r0 * static_cast<f32>(mesh.triangleCount), mesh.triangleCount);
    const MeshAliasSlot& slot = mesh.alias[index];
    const u32 picked = slot.proba <= r3 ? slot.other : index;
    return picked < mesh.triangleCount ? picked : index;
}

MeshSurfaceSample sampleMeshSurface(const MeshShapeData& mesh, MeshSamplingDistribution mode,
                                    f32 r0, f32 r1, f32 r2, f32 r3) noexcept {
    MeshSurfaceSample out;
    if (mesh.triangleCount == 0U || mesh.positions.empty()) {
        return out;
    }

    out.triangle = sampleMeshTriangle(mesh, mode, r0, r3);

    const f32 coeff = std::sqrt(r1 < 0.0F ? 0.0F : r1);
    const f32 u = 1.0F - coeff;
    const f32 v = coeff * r2;
    out.u = u;
    out.v = v;

    const MeshParametricCoords pc{out.triangle, u, v};
    interpolateStream(mesh, mesh.positions, 3U, pc, out.position.data());
    interpolateStream(mesh, mesh.normals, 3U, pc, out.normal.data());
    interpolateStream(mesh, mesh.texcoords(), 2U, pc, out.texcoord.data());
    interpolateStream(mesh, mesh.colors(), 4U, pc, out.color.data());
    return out;
}

u8 sampleMeshField(const MeshShapeData& mesh, MeshField field, i32 streamIndex,
                   const MeshParametricCoords& pc, f32* out) noexcept {
    const u8 comps = meshFieldComponents(field, streamIndex);
    std::span<const f32> stream;
    u32 streamComps = 3U;
    switch (field) {
    case MeshField::Position:
        stream = mesh.positions;
        break;
    case MeshField::Normal:
        stream = mesh.normals;
        break;
    case MeshField::Tangent:
        stream = mesh.tangents;
        streamComps = 4U;
        break;
    case MeshField::Texcoord:
        stream = mesh.texcoords(streamIndex);
        streamComps = 2U;
        break;
    case MeshField::Color:
        stream = mesh.colors(streamIndex);
        streamComps = 4U;
        break;
    case MeshField::Velocity:
        for (u8 c = 0U; c < comps; ++c) {
            out[c] = 0.0F;
        }
        return comps;
    }
    if (stream.empty()) {
        return 0U;
    }
    std::array<f32, 4> tmp{};
    interpolateStream(mesh, stream, streamComps, pc, tmp.data());
    for (u8 c = 0U; c < comps; ++c) {
        out[c] = tmp[c];
    }
    return comps;
}

u32 buildMeshBvh(const MeshShapeData& mesh, std::span<MeshBvhNode> nodes,
                 std::span<u32> triangleOrder) noexcept {
    const u32 triCount = mesh.triangleCount;
    if (triCount == 0U || nodes.empty() || triangleOrder.size() < triCount) {
        return 0U;
    }
    for (u32 i = 0U; i < triCount; ++i) {
        triangleOrder[i] = i;
    }

    std::vector<std::array<f32, 3>> centroid(triCount);
    std::vector<std::array<f32, 6>> bounds(triCount);
    for (u32 t = 0U; t < triCount; ++t) {
        const auto idx = triangleIndices(mesh, t);
        std::array<f32, 3> lo{kInf, kInf, kInf};
        std::array<f32, 3> hi{-kInf, -kInf, -kInf};
        std::array<f32, 3> sum{};
        for (const u32 vi : idx) {
            const auto p = vertexPos(mesh, vi);
            for (u32 c = 0U; c < 3U; ++c) {
                lo[c] = std::min(lo[c], p[c]);
                hi[c] = std::max(hi[c], p[c]);
                sum[c] += p[c];
            }
        }
        for (u32 c = 0U; c < 3U; ++c) {
            centroid[t][c] = sum[c] * (1.0F / 3.0F);
            bounds[t][c] = lo[c];
            bounds[t][3U + c] = hi[c];
        }
    }

    constexpr u32 kLeafSize = 8U;
    u32 nodeCount = 1U;

    struct Range {
        u32 node;
        u32 first;
        u32 count;
    };
    std::vector<Range> stack;
    stack.push_back({0U, 0U, triCount});

    while (!stack.empty()) {
        const Range r = stack.back();
        stack.pop_back();
        MeshBvhNode& node = nodes[r.node];
        std::array<f32, 3> lo{kInf, kInf, kInf};
        std::array<f32, 3> hi{-kInf, -kInf, -kInf};
        std::array<f32, 3> cLo{kInf, kInf, kInf};
        std::array<f32, 3> cHi{-kInf, -kInf, -kInf};
        for (u32 i = r.first; i < r.first + r.count; ++i) {
            const u32 t = triangleOrder[i];
            for (u32 c = 0U; c < 3U; ++c) {
                lo[c] = std::min(lo[c], bounds[t][c]);
                hi[c] = std::max(hi[c], bounds[t][3U + c]);
                cLo[c] = std::min(cLo[c], centroid[t][c]);
                cHi[c] = std::max(cHi[c], centroid[t][c]);
            }
        }
        node.boundsMin = lo;
        node.boundsMax = hi;

        if (r.count <= kLeafSize || nodeCount + 2U > nodes.size()) {
            node.firstTriangle = r.first;
            node.triangleCount = r.count;
            continue;
        }

        u32 axis = 0U;
        f32 extent = cHi[0] - cLo[0];
        for (u32 c = 1U; c < 3U; ++c) {
            if (cHi[c] - cLo[c] > extent) {
                extent = cHi[c] - cLo[c];
                axis = c;
            }
        }
        const auto begin = triangleOrder.begin() + static_cast<std::ptrdiff_t>(r.first);
        const u32 leftCount = r.count / 2U;
        std::nth_element(begin, begin + static_cast<std::ptrdiff_t>(leftCount),
                         begin + static_cast<std::ptrdiff_t>(r.count),
                         [&](u32 a, u32 b) { return centroid[a][axis] < centroid[b][axis]; });

        node.triangleCount = 0U;
        const u32 left = nodeCount++;
        const u32 right = nodeCount++;
        node.leftChild = left;
        node.rightChild = right;
        stack.push_back({right, r.first + leftCount, r.count - leftCount});
        stack.push_back({left, r.first, leftCount});
    }
    return nodeCount;
}

#if defined(CORNFLAKES_MESH_PROBE)
std::atomic<std::uint64_t> g_meshQueries{0};
std::atomic<std::uint64_t> g_meshTriTests{0};
std::atomic<std::uint64_t> g_meshNodesVisited{0};
std::atomic<std::uint64_t> g_meshTriCount{0};
#endif

namespace {

using Vec3 = std::array<f32, 3>;

Vec3 sub3(const Vec3& a, const Vec3& b) noexcept {
    return {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
}
f32 dot3v(const Vec3& a, const Vec3& b) noexcept {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}
Vec3 cross3v(const Vec3& a, const Vec3& b) noexcept {
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
Vec3 normalize3v(const Vec3& v) noexcept {
    const f32 len = std::sqrt(dot3v(v, v));
    if (!(len > 0.0F)) {
        return {0.0F, 0.0F, 0.0F};
    }
    const f32 inv = 1.0F / len;
    return {v[0] * inv, v[1] * inv, v[2] * inv};
}

f32 lowPrecisionRcp(f32 x) noexcept {
#if defined(__SSE__) || defined(_M_X64) || defined(_M_AMD64)
    return _mm_cvtss_f32(_mm_rcp_ss(_mm_set_ss(x)));
#else
    return 1.0F / x;
#endif
}

i32 fpToIntFriendlyBits(f32 v) noexcept {
    u32 bits = 0U;
    std::memcpy(&bits, &v, sizeof(bits));
    if ((bits & 0x80000000U) != 0U) {
        bits ^= 0x7FFFFFFFU;
    }
    i32 out = 0;
    std::memcpy(&out, &bits, sizeof(out));
    return out;
}

constexpr u32 kNoTriangle = 0xFFFFFFFFU;

struct ProjClosest {
    f32 distSq = kInf;
    Vec3 reproj{};
    u32 triangle = kNoTriangle;
};

void considerProjTriangle(const MeshProjTriangle& t, const Vec3& pos, u32 id,
                          ProjClosest& best) noexcept {
    const Vec3 bari{t.bari[0], t.bari[1], t.bari[2]};
    const Vec3 pb = sub3(pos, bari);

    std::array<f32, 3> dist{};
    std::array<f32, 3> den{};
    for (std::size_t k = 0; k < 3U; ++k) {
        dist[k] = t.nx[k] * (pos[0] - t.vx[k]) + t.ny[k] * (pos[1] - t.vy[k]) +
                  t.nz[k] * (pos[2] - t.vz[k]);
        den[k] = t.nx[k] * pb[0] + t.ny[k] * pb[1] + t.nz[k] * pb[2];
    }

    f32 maxMul = 0.0F;
    for (std::size_t k = 0; k < 3U; ++k) {
        const f32 mul = dist[k] < 0.0F ? 0.0F : dist[k] * lowPrecisionRcp(den[k]);
        maxMul = std::max(maxMul, mul);
    }

    const Vec3 norm{t.norm[0], t.norm[1], t.norm[2]};
    const f32 d = dot3v(norm, pb);
    const Vec3 np{norm[0] * d, norm[1] * d, norm[2] * d};

    const Vec3 reproj{(np[0] - pb[0]) * maxMul - np[0], (np[1] - pb[1]) * maxMul - np[1],
                      (np[2] - pb[2]) * maxMul - np[2]};
    const f32 distSq = dot3v(reproj, reproj);
    if (distSq < best.distSq) {
        best.distSq = distSq;
        best.reproj = reproj;
        best.triangle = id;
    }
}

bool mapWithinFirstOverlaps(const MeshShapeData& mesh, const Vec3& boxMin, const Vec3& boxMax,
                            const Vec3& pos, ProjClosest& best) noexcept {
    std::array<i32, 3> lo{};
    std::array<i32, 3> hi{};
    for (std::size_t i = 0; i < 3U; ++i) {
        lo[i] = fpToIntFriendlyBits(boxMin[i]);
        hi[i] = fpToIntFriendlyBits(boxMax[i]);
    }

    bool found = false;
    std::array<u32, 128> stack{};
    u32 depth = 0U;
    u32 node = 0U;

    for (;;) {
        while (node < mesh.kdNodes.size() && !mesh.kdNodes[node].isLeaf()) {
            const auto& n = mesh.kdNodes[node];
            const u32 axis = n.axis();
            if (axis >= 3U) {
                break;
            }
            const i32 split = fpToIntFriendlyBits(n.splitPos);
            const bool minSide = lo[axis] < split;
            const bool maxSide = hi[axis] < split;
            if (minSide == maxSide) {
                node = minSide ? node + 1U : node + n.secondChildOffsetNodes();
            } else {
                if (depth >= stack.size()) {
                    break;
                }
                stack[depth++] = node + n.secondChildOffsetNodes();
                node = node + 1U;
            }
        }

        if (node < mesh.kdNodes.size() && mesh.kdNodes[node].isLeaf()) {
            const u32 rec = mesh.kdNodes[node].leafElement();
            if (rec < mesh.kdLeaves.size()) {
                const u32 count = mesh.kdLeaves[rec];
                if (count != 0U) {
                    found = true;
                    for (u32 i = 0U; i < count; ++i) {
                        const std::size_t at = static_cast<std::size_t>(rec) + 1U + i;
                        if (at >= mesh.kdLeaves.size()) {
                            break;
                        }
                        const u32 tri = mesh.kdLeaves[at];
                        if (tri < mesh.projTriangles.size()) {
                            considerProjTriangle(mesh.projTriangles[tri], pos, tri, best);
                        }
                    }
                }
            }
        }

        if (depth == 0U) {
            break;
        }
        node = stack[--depth];
    }
    return found;
}

}

MeshProjConfig meshProjConfigFor(const std::array<f32, 3>& bboxMin,
                                 const std::array<f32, 3>& bboxMax) noexcept {
    MeshProjConfig cfg;
    constexpr f32 kFirstSearchRatio = 0.0020000001F;
    constexpr f32 kSearchRatioIncrementStep = 0.0040000002F;
    constexpr f32 kSearchRatioGrowthFactor = 1.0F;

    f32 boxMaxDist = 0.0F;
    for (std::size_t i = 0; i < 3U; ++i) {
        boxMaxDist = std::max(boxMaxDist, std::fabs(bboxMax[i] - bboxMin[i]));
    }
    cfg.delta = boxMaxDist * kFirstSearchRatio;
    cfg.deltaStep = boxMaxDist * kSearchRatioIncrementStep;
    cfg.deltaGrow = kSearchRatioGrowthFactor;
    if (!(cfg.deltaStep > 0.0F)) {
        cfg.maxIter = 0U;
        return cfg;
    }
    const f32 iters = std::pow(boxMaxDist / cfg.deltaStep, 1.0F / cfg.deltaGrow);
    cfg.maxIter = static_cast<u32>(static_cast<i32>(iters)) + 1U;
    return cfg;
}

void buildMeshProjTriangles(std::span<const f32> positions, std::span<const u32> indices,
                            std::span<MeshProjTriangle> out) noexcept {
    const auto vertexAt = [&](u32 i) -> Vec3 {
        const std::size_t o = static_cast<std::size_t>(i) * 3U;
        if (o + 2U >= positions.size()) {
            return {0.0F, 0.0F, 0.0F};
        }
        return {positions[o], positions[o + 1U], positions[o + 2U]};
    };

    const std::size_t count = std::min(out.size(), indices.size() / 3U);
    for (std::size_t t = 0; t < count; ++t) {
        const Vec3 v0 = vertexAt(indices[t * 3U]);
        const Vec3 v1 = vertexAt(indices[t * 3U + 1U]);
        const Vec3 v2 = vertexAt(indices[t * 3U + 2U]);

        const Vec3 e0 = sub3(v1, v0);
        const Vec3 e1 = sub3(v2, v1);
        const Vec3 e2 = sub3(v0, v2);
        const Vec3 n = normalize3v(cross3v(e0, e2));
        const Vec3 n0 = normalize3v(cross3v(n, e0));
        const Vec3 n1 = normalize3v(cross3v(n, e1));
        const Vec3 n2 = normalize3v(cross3v(n, e2));

        const Vec3 demie0{e0[0] * 0.5F, e0[1] * 0.5F, e0[2] * 0.5F};
        const Vec3 lhs{v0[0] + demie0[0], v0[1] + demie0[1], v0[2] + demie0[2]};
        constexpr f32 kThird = 0.33333334F;
        const Vec3 rhs{(e2[0] + demie0[0]) * kThird, (e2[1] + demie0[1]) * kThird,
                       (e2[2] + demie0[2]) * kThird};
        const Vec3 bari = sub3(lhs, rhs);

        const Vec3 ed0 = sub3(v0, v2);
        const Vec3 ed2 = sub3(v1, v2);
        const f32 d00 = dot3v(ed0, ed0);
        const f32 d01 = dot3v(ed0, ed2);
        const f32 d11 = dot3v(ed2, ed2);
        const f32 invDen = 1.0F / (d00 * d11 - d01 * d01);

        auto& o = out[t];
        o.vx = {v0[0], v1[0], v2[0], bari[0]};
        o.vy = {v0[1], v1[1], v2[1], bari[1]};
        o.vz = {v0[2], v1[2], v2[2], bari[2]};
        o.nx = {n0[0], n1[0], n2[0], invDen};
        o.ny = {n0[1], n1[1], n2[1], d11};
        o.nz = {n0[2], n1[2], n2[2], d01};
        o.norm = {n[0], n[1], n[2], d00};
        o.bari = {bari[0], bari[1], bari[2], 0.0F};
    }
}

#if defined(CORNFLAKES_MESH_PROBE)
std::atomic<std::uint64_t> g_engineProjOk{0};
std::atomic<std::uint64_t> g_engineProjNoTree{0};
std::atomic<std::uint64_t> g_engineProjMiss{0};
std::atomic<std::uint64_t> g_engineProjIters{0};
std::atomic<std::uint64_t> g_projCompared{0};
std::atomic<std::uint64_t> g_projSameTri{0};
std::atomic<std::uint64_t> g_projDistSum{0};
std::atomic<std::uint64_t> g_projDistMax{0};
#endif

bool projectMeshPointEngine(const MeshShapeData& mesh, const std::array<f32, 3>& point,
                            MeshParametricCoords& out) noexcept {
    if (!mesh.hasEngineProjection()) {
#if defined(CORNFLAKES_MESH_PROBE)
        g_engineProjNoTree.fetch_add(1, std::memory_order_relaxed);
#endif
        return false;
    }
    Vec3 pos{};
    for (std::size_t i = 0; i < 3U; ++i) {
        const auto& m = mesh.engineToFile[i];
        pos[i] = m.sign * point[m.source < 3U ? m.source : i];
    }
    if (!std::isfinite(pos[0]) || !std::isfinite(pos[1]) || !std::isfinite(pos[2])) {
        return false;
    }

    Vec3 testpos{};
    for (std::size_t i = 0; i < 3U; ++i) {
        testpos[i] = std::min(std::max(pos[i], mesh.kdBboxMin[i]), mesh.kdBboxMax[i]);
    }

    ProjClosest best;
    f32 ddelta = mesh.projConfig.delta;
    f32 step = mesh.projConfig.deltaStep;
    bool found = false;
#if defined(CORNFLAKES_MESH_PROBE)
    u32 iterations = 0U;
#endif
    for (u32 c = 0U; c < mesh.projConfig.maxIter && !found; ++c) {
#if defined(CORNFLAKES_MESH_PROBE)
        ++iterations;
#endif
        const Vec3 boxMin{testpos[0] - ddelta, testpos[1] - ddelta, testpos[2] - ddelta};
        const Vec3 boxMax{testpos[0] + ddelta, testpos[1] + ddelta, testpos[2] + ddelta};
        found = mapWithinFirstOverlaps(mesh, boxMin, boxMax, pos, best);
        step *= mesh.projConfig.deltaGrow;
        ddelta += step;
    }
#if defined(CORNFLAKES_MESH_PROBE)
    g_engineProjIters.fetch_add(iterations, std::memory_order_relaxed);
#endif
    if (!found || best.triangle == kNoTriangle) {
#if defined(CORNFLAKES_MESH_PROBE)
        g_engineProjMiss.fetch_add(1, std::memory_order_relaxed);
#endif
        return false;
    }
#if defined(CORNFLAKES_MESH_PROBE)
    g_engineProjOk.fetch_add(1, std::memory_order_relaxed);
    {
        MeshParametricCoords exact{};
        if (projectMeshPoint(mesh, point, exact)) {
            const auto at = [&](const MeshParametricCoords& pc) {
                const std::size_t o = static_cast<std::size_t>(pc.triangle) * 3U;
                Vec3 acc{};
                if (o + 2U < mesh.indices.size()) {
                    const f32 w[3] = {pc.u, pc.v, 1.0F - pc.u - pc.v};
                    for (std::size_t k = 0; k < 3U; ++k) {
                        const std::size_t vi = static_cast<std::size_t>(mesh.indices[o + k]) * 3U;
                        if (vi + 2U < mesh.positions.size()) {
                            for (std::size_t c = 0; c < 3U; ++c) {
                                acc[c] += mesh.positions[vi + c] * w[k];
                            }
                        }
                    }
                }
                return acc;
            };
            const Vec3 pe = at(out);
            const Vec3 px = at(exact);
            const f32 d = std::sqrt(dot3v(sub3(pe, px), sub3(pe, px)));
            if (out.triangle == exact.triangle) {
                g_projSameTri.fetch_add(1, std::memory_order_relaxed);
            }
            g_projDistSum.fetch_add(static_cast<std::uint64_t>(d * 1000000.0F),
                                    std::memory_order_relaxed);
            auto prev = g_projDistMax.load(std::memory_order_relaxed);
            const auto cur = static_cast<std::uint64_t>(d * 1000000.0F);
            while (cur > prev &&
                   !g_projDistMax.compare_exchange_weak(prev, cur, std::memory_order_relaxed)) {
            }
            g_projCompared.fetch_add(1, std::memory_order_relaxed);
        }
    }
#endif

    const auto& t = mesh.projTriangles[best.triangle];
    const Vec3 locProj{pos[0] + best.reproj[0], pos[1] + best.reproj[1], pos[2] + best.reproj[2]};
    const Vec3 v0{t.vx[0], t.vy[0], t.vz[0]};
    const Vec3 v1{t.vx[1], t.vy[1], t.vz[1]};
    const Vec3 v2{t.vx[2], t.vy[2], t.vz[2]};
    const f32 invDen = t.nx[3];
    const f32 d11 = t.ny[3];
    const f32 d01 = t.nz[3];
    const f32 d00 = t.norm[3];

    const Vec3 rel = sub3(locProj, v2);
    const Vec3 vec{rel[0] * invDen, rel[1] * invDen, rel[2] * invDen};
    const Vec3 e0 = sub3(v0, v2);
    const Vec3 e2 = sub3(v1, v2);
    const f32 d02 = dot3v(e0, vec);
    const f32 d12 = dot3v(e2, vec);

    out.triangle = best.triangle;
    const f32 u = d02 * d11 - d01 * d12;
    const f32 v = d12 * d00 - d01 * d02;
    out.u = u;
    out.v = v;
    if (mesh.projWindingFlipped) {
        out.v = 1.0F - u - v;
    }
    return true;
}

bool projectMeshPoint(const MeshShapeData& mesh, const std::array<f32, 3>& point,
                      MeshParametricCoords& out) noexcept {
    if (mesh.triangleCount == 0U || mesh.positions.empty()) {
        return false;
    }
#if defined(CORNFLAKES_MESH_PROBE)
    g_meshQueries.fetch_add(1, std::memory_order_relaxed);
    g_meshTriCount.store(mesh.triangleCount, std::memory_order_relaxed);
#endif
    f32 bestDistSq = kInf;
    const auto consider = [&](u32 t) {
#if defined(CORNFLAKES_MESH_PROBE)
        g_meshTriTests.fetch_add(1, std::memory_order_relaxed);
#endif
        f32 u = 0.0F;
        f32 v = 0.0F;
        const f32 d = closestOnTriangle(mesh, t, point, u, v);
        if (d < bestDistSq) {
            bestDistSq = d;
            out.triangle = t;
            out.u = u;
            out.v = v;
        }
    };
    const auto scanAll = [&]() {
        for (u32 t = 0U; t < mesh.triangleCount; ++t) {
            consider(t);
        }
    };

    if (mesh.bvh.empty() || mesh.bvhTriangles.size() < mesh.triangleCount) {
        scanAll();
        return true;
    }

    std::array<u32, 64> stack{};
    u32 depth = 0U;
    stack[depth++] = 0U;
    while (depth > 0U) {
        const u32 ni = stack[--depth];
        if (ni >= mesh.bvh.size()) {
            continue;
        }
        const MeshBvhNode& node = mesh.bvh[ni];
#if defined(CORNFLAKES_MESH_PROBE)
        g_meshNodesVisited.fetch_add(1, std::memory_order_relaxed);
#endif
        if (boxDistanceSq(node, point) >= bestDistSq) {
            continue;
        }
        if (node.triangleCount != 0U) {
            for (u32 i = 0U; i < node.triangleCount; ++i) {
                const std::size_t o = static_cast<std::size_t>(node.firstTriangle) + i;
                if (o < mesh.bvhTriangles.size()) {
                    consider(mesh.bvhTriangles[o]);
                }
            }
            continue;
        }
        if (depth + 2U > stack.size()) {
            scanAll();
            return true;
        }

        const u32 l = node.leftChild;
        const u32 r = node.rightChild;
        const bool lOk = l < mesh.bvh.size();
        const bool rOk = r < mesh.bvh.size();
        const f32 dl = lOk ? boxDistanceSq(mesh.bvh[l], point) : kInf;
        const f32 dr = rOk ? boxDistanceSq(mesh.bvh[r], point) : kInf;

        const u32 nearIdx = dl <= dr ? l : r;
        const u32 farIdx = dl <= dr ? r : l;
        const f32 nearD = dl <= dr ? dl : dr;
        const f32 farD = dl <= dr ? dr : dl;

        if (farD < bestDistSq) {
            stack[depth++] = farIdx;
        }
        if (nearD < bestDistSq) {
            stack[depth++] = nearIdx;
        }
    }
    return true;
}

#if defined(CORNFLAKES_MESH_PROBE)
void reportMeshProbe() noexcept {
    const auto q = g_meshQueries.load();
    const auto t = g_meshTriTests.load();
    const auto n = g_meshNodesVisited.load();
    const auto engineTotal = g_engineProjOk.load() + g_engineProjMiss.load();
    std::printf("[mesh] engineProj ok=%llu miss=%llu noTree=%llu iters/query=%.2f\n",
                static_cast<unsigned long long>(g_engineProjOk.load()),
                static_cast<unsigned long long>(g_engineProjMiss.load()),
                static_cast<unsigned long long>(g_engineProjNoTree.load()),
                engineTotal == 0U ? 0.0
                                  : static_cast<double>(g_engineProjIters.load()) /
                                        static_cast<double>(engineTotal));
    std::printf("[mesh] queries=%llu triCount=%llu triTests/query=%.1f nodes/query=%.1f\n",
                static_cast<unsigned long long>(q),
                static_cast<unsigned long long>(g_meshTriCount.load()),
                static_cast<double>(t) / static_cast<double>(q),
                static_cast<double>(n) / static_cast<double>(q));
}
#endif

}
