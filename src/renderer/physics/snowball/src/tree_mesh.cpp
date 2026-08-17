#include "snowball/tree_mesh.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <numeric>

namespace snowball {
namespace {

/// The reciprocal Create uses: the engine's estimate-plus-Newton, masked to zero wherever the
/// input is within `zeroSafe` of zero rather than producing an infinity.
Vec4 GuardedRcp(const Vec4& v) {
    const Vec4 r = Rcp(v);
    const f32 guard = constants::kZeroSafe.x;
    return {std::abs(v.x) > guard ? r.x : 0.0f, std::abs(v.y) > guard ? r.y : 0.0f,
            std::abs(v.z) > guard ? r.z : 0.0f, std::abs(v.w) > guard ? r.w : 0.0f};
}

Vec4 Dequantise(const i16 (&q)[3], const Vec4& scale) {
    return {static_cast<f32>(q[0]) * scale.x, static_cast<f32>(q[1]) * scale.y,
            static_cast<f32>(q[2]) * scale.z, 0.0f};
}

bool Overlaps3(const Vec4& lowerA, const Vec4& upperA, const Vec4& lowerB, const Vec4& upperB) {
    return upperA.x >= lowerB.x && upperA.y >= lowerB.y && upperA.z >= lowerB.z &&
           upperB.x >= lowerA.x && upperB.y >= lowerA.y && upperB.z >= lowerA.z;
}

Vec4 Min3(const Vec4& a, const Vec4& b) {
    return {std::min(a.x, b.x), std::min(a.y, b.y), std::min(a.z, b.z), 0.0f};
}

Vec4 Normalize3(const Vec4& v) { return v * Rsqrt(Vec4::Splat(LengthSquared3(v))).x; }

Vec4 Max3(const Vec4& a, const Vec4& b) {
    return {std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z), 0.0f};
}

}  // namespace

void TreeMesh::Create(const MeshData& data, const Vec4& scale) {
    data_ = data;
    scale_ = scale;
    inverseScale_ = GuardedRcp(scale);
    triangleCount_ = static_cast<i32>(data.triangles.size());
}

void TreeMesh::ApplyScale(const Vec4& scale) {
    scale_ = scale_ * scale;
    // An exact divide, where Create refines an estimate. Unifying the two would be an
    // improvement and a bit-level behaviour change; the mismatch is deliberate.
    inverseScale_ = {inverseScale_.x / scale.x, inverseScale_.y / scale.y,
                     inverseScale_.z / scale.z, inverseScale_.w / scale.w};
}

Triangle TreeMesh::GetTriangle(i32 index) const {
    Triangle t;
    if (index < 0 || index >= triangleCount_) {
        return t;
    }
    const MeshTriangle& source = data_.triangles[static_cast<usize>(index)];
    const auto place = [&](i32 vertex) {
        return (data_.vertices[static_cast<usize>(vertex)] + data_.offset) * scale_;
    };
    t.v1 = place(source.vertex[0]);
    t.v2 = place(source.vertex[1]);
    t.v3 = place(source.vertex[2]);
    return t;
}

Triangle TreeMesh::GetTriangleFull(i32 index) const {
    Triangle t = GetTriangle(index);
    if (index < 0 || index >= triangleCount_) {
        return t;
    }
    const MeshTriangle& source = data_.triangles[static_cast<usize>(index)];
    const Vec4 corner[3] = {t.v1, t.v2, t.v3};
    for (i32 k = 0; k < 3; ++k) {
        const i32 neighbour = source.adjacent[k];
        t.adjacentId[static_cast<usize>(k)] = neighbour;
        t.vertexId[static_cast<usize>(k)] = source.vertex[k];
        if (neighbour < 0) {
            // An open edge still gets a vertex written, and it is a corner of this triangle --
            // so the edge plane built from it degenerates rather than pointing somewhere
            // arbitrary. Which corner is not the obvious one: edge k falls back to corner k+2.
            t.adjacent[static_cast<usize>(k)] = corner[(k + 2) % 3];
            t.hasAdjacent[static_cast<usize>(k)] = false;
        } else {
            t.adjacent[static_cast<usize>(k)] =
                (data_.vertices[static_cast<usize>(neighbour)] + data_.offset) * scale_;
            t.hasAdjacent[static_cast<usize>(k)] = true;
        }
    }
    t.index = index;
    t.material = source.material;
    return t;
}

void TreeMesh::Query(const Aabb& bounds, std::vector<i32>& out) const {
    if (data_.nodes.empty() || data_.treeDepth >= 128) {
        return;
    }
    // Into the mesh's own space once, rather than lifting every node out of it.
    const Vec4 queryLower = bounds.lower * inverseScale_ - data_.offset;
    const Vec4 queryUpper = bounds.upper * inverseScale_ - data_.offset;

    const MeshNode* stack[128];
    i32 top = 0;
    stack[top++] = data_.nodes.data();

    while (top > 0) {
        const MeshNode* node = stack[top - 1];
        const Vec4 nodeLower = Dequantise(node->lower, data_.quantScale);
        const Vec4 nodeUpper = Dequantise(node->upper, data_.quantScale);
        if (!Overlaps3(nodeLower, nodeUpper, queryLower, queryUpper)) {
            --top;
            continue;
        }
        if (!node->IsLeaf()) {
            // The slot being examined becomes the right child and the left child goes on top of
            // it, so the left subtree is the one descended into first.
            stack[top - 1] = node + node->Payload();
            stack[top++] = node + 1;
            continue;
        }
        --top;
        const i32 first = node->Payload();
        for (i32 i = 0; i < node->TriangleCount(); ++i) {
            const i32 index = first + i;
            const MeshTriangle& source = data_.triangles[static_cast<usize>(index)];
            const Vec4& p0 = data_.vertices[static_cast<usize>(source.vertex[0])];
            const Vec4& p1 = data_.vertices[static_cast<usize>(source.vertex[1])];
            const Vec4& p2 = data_.vertices[static_cast<usize>(source.vertex[2])];
            if (Overlaps3(Min3(Min3(p0, p1), p2), Max3(Max3(p0, p1), p2), queryLower,
                          queryUpper)) {
                out.push_back(index);
            }
        }
    }
}

bool TreeMesh::RayCast(const RayCastInput& input, const Transform& xf,
                       RayCastOutput& out) const {
    if (data_.nodes.empty() || data_.treeDepth >= 128) {
        return false;
    }
    // The ray goes down into the mesh's quantised space once, like a query AABB does.
    const Mtx rotation = RotationMatrix(xf.rotation);
    const Mtx inverse = Transpose3(rotation);
    const Vec4 from = inverse.Transform(input.p1 - xf.position) * inverseScale_ - data_.offset;
    const Vec4 to = inverse.Transform(input.p2 - xf.position) * inverseScale_ - data_.offset;
    const Vec4 direction = to - from;
    const Vec4 absDirection = Max3(-direction, direction);
    const f32 axisSign[3] = {direction.x, direction.y, direction.z};

    f32 maxFraction = input.maxFraction;
    bool hit = false;

    const MeshNode* stack[128];
    i32 top = 0;
    stack[top++] = data_.nodes.data();

    while (top > 0) {
        const MeshNode* node = stack[--top];
        const Vec4 lower = Dequantise(node->lower, data_.quantScale);
        const Vec4 upper = Dequantise(node->upper, data_.quantScale);
        // The ray's own bounds first, then the three cross-product axes of a segment/box
        // separating-axis test -- which is what makes a long thin ray cheap. Bounds alone would
        // accept every node the ray's bounding box straddles, and that is most of the tree.
        const Vec4 end = from + direction * maxFraction;
        if (!Overlaps3(lower, upper, Min3(from, end), Max3(from, end))) {
            continue;
        }
        const Vec4 halfExtent = (upper - lower) * 0.5f;
        const Vec4 toCentre = from - (lower + upper) * 0.5f;
        const Vec4 arm = Cross3(toCentre, direction);
        const f32 reach[3] = {
            halfExtent.y * absDirection.z + halfExtent.z * absDirection.y,
            halfExtent.z * absDirection.x + halfExtent.x * absDirection.z,
            halfExtent.x * absDirection.y + halfExtent.y * absDirection.x};
        const f32 span[3] = {std::abs(arm.x), std::abs(arm.y), std::abs(arm.z)};
        if (span[0] > reach[0] || span[1] > reach[1] || span[2] > reach[2]) {
            continue;
        }

        if (!node->IsLeaf()) {
            // Bits 0..1 are the split axis, and their only use is here: descend the side the ray
            // starts on first, so the far child meets an already-shortened maxFraction.
            const MeshNode* left = node + 1;
            const MeshNode* right = node + node->Payload();
            const bool farSideFirst = axisSign[node->packed & 3] > 0.0f;
            stack[top++] = farSideFirst ? left : right;
            stack[top++] = farSideFirst ? right : left;
            continue;
        }

        const i32 first = node->Payload();
        for (i32 i = 0; i < node->TriangleCount(); ++i) {
            const i32 index = first + i;
            const MeshTriangle& source = data_.triangles[static_cast<usize>(index)];
            if ((input.mask & source.mask) == 0) {
                continue;
            }
            Triangle t;
            t.v1 = data_.vertices[static_cast<usize>(source.vertex[0])];
            t.v2 = data_.vertices[static_cast<usize>(source.vertex[1])];
            t.v3 = data_.vertices[static_cast<usize>(source.vertex[2])];
            RayCastOutput local;
            if (!TriangleRayCast(t, from, to, maxFraction, local)) {
                continue;
            }
            maxFraction = local.fraction;
            out.fraction = local.fraction;
            // The normal is taken from the *scaled* corners, not the quantised-space ones the
            // intersection ran in -- a non-uniform scale tilts it.
            out.normal = rotation.Transform(
                Normalize3(Cross3((t.v2 - t.v1) * scale_, (t.v3 - t.v1) * scale_)));
            out.triangle = index;
            out.material = source.material;
            hit = true;
        }
    }
    return hit;
}

Aabb TreeMesh::GetAabb(const Transform& xf, f32 radius) const {
    if (data_.vertices.empty()) {
        return Aabb{-constants::kOne, constants::kOne};
    }
    Aabb bounds{constants::kMaxFloat, -constants::kMaxFloat};
    const Mtx rotation = RotationMatrix(xf.rotation);
    for (const Vec4& vertex : data_.vertices) {
        const Vec4 p = rotation.Transform((vertex + data_.offset) * scale_) + xf.position;
        bounds.lower = Min3(bounds.lower, p);
        bounds.upper = Max3(bounds.upper, p);
    }
    bounds.lower = bounds.lower - Vec4::Splat(radius);
    bounds.upper = bounds.upper + Vec4::Splat(radius);
    return bounds;
}

namespace {

/// Quantise toward the outside, so a node never reports bounds tighter than its contents.
i16 QuantiseLow(f32 v, f32 scale) {
    const f32 q = std::floor(v / scale);
    return static_cast<i16>(std::clamp(q, -32768.0f, 32767.0f));
}

i16 QuantiseHigh(f32 v, f32 scale) {
    const f32 q = std::ceil(v / scale);
    return static_cast<i16>(std::clamp(q, -32768.0f, 32767.0f));
}

struct BuildContext {
    std::span<const Vec4> vertices;
    const std::vector<MeshTriangle>* triangles;
    Vec4 quantScale;
    Vec4 offset;
    std::vector<i32> order;
    std::vector<MeshNode> nodes;
};

void BoundsOf(const BuildContext& ctx, i32 begin, i32 end, Vec4& lower, Vec4& upper) {
    lower = constants::kMaxFloat;
    upper = -constants::kMaxFloat;
    for (i32 i = begin; i < end; ++i) {
        const MeshTriangle& t = (*ctx.triangles)[static_cast<usize>(ctx.order[static_cast<usize>(i)])];
        for (i32 k = 0; k < 3; ++k) {
            const Vec4& p = ctx.vertices[static_cast<usize>(t.vertex[k])];
            lower = Min3(lower, p);
            upper = Max3(upper, p);
        }
    }
}

Vec4 CentroidOf(const BuildContext& ctx, i32 triangle) {
    const MeshTriangle& t = (*ctx.triangles)[static_cast<usize>(triangle)];
    const Vec4 sum = ctx.vertices[static_cast<usize>(t.vertex[0])] +
                     ctx.vertices[static_cast<usize>(t.vertex[1])] +
                     ctx.vertices[static_cast<usize>(t.vertex[2])];
    return sum * constants::kOneThird.x;
}

/// Returns the index of the node it wrote. Children are emitted so that the left one is
/// immediately after its parent, which is what lets the encoding leave its index implicit.
i32 BuildRange(BuildContext& ctx, i32 begin, i32 end) {
    const i32 self = static_cast<i32>(ctx.nodes.size());
    ctx.nodes.emplace_back();

    Vec4 lower;
    Vec4 upper;
    BoundsOf(ctx, begin, end, lower, upper);

    const i32 count = end - begin;
    if (count <= kMaxLeafTriangles) {
        MeshNode& leaf = ctx.nodes[static_cast<usize>(self)];
        leaf.lower[0] = QuantiseLow(lower.x, ctx.quantScale.x);
        leaf.lower[1] = QuantiseLow(lower.y, ctx.quantScale.y);
        leaf.lower[2] = QuantiseLow(lower.z, ctx.quantScale.z);
        leaf.upper[0] = QuantiseHigh(upper.x, ctx.quantScale.x);
        leaf.upper[1] = QuantiseHigh(upper.y, ctx.quantScale.y);
        leaf.upper[2] = QuantiseHigh(upper.z, ctx.quantScale.z);
        leaf.packed = MeshNode::Pack(count, begin);
        return self;
    }

    const Vec4 extent = upper - lower;
    const i32 axis = extent.x > extent.y ? (extent.x > extent.z ? 0 : 2)
                                        : (extent.y > extent.z ? 1 : 2);
    const auto key = [&](i32 triangle) {
        const Vec4 c = CentroidOf(ctx, triangle);
        return axis == 0 ? c.x : (axis == 1 ? c.y : c.z);
    };
    const i32 middle = begin + count / 2;
    std::nth_element(ctx.order.begin() + begin, ctx.order.begin() + middle,
                     ctx.order.begin() + end,
                     [&](i32 a, i32 b) { return key(a) < key(b); });

    BuildRange(ctx, begin, middle);
    const i32 right = BuildRange(ctx, middle, end);

    MeshNode& node = ctx.nodes[static_cast<usize>(self)];
    node.lower[0] = QuantiseLow(lower.x, ctx.quantScale.x);
    node.lower[1] = QuantiseLow(lower.y, ctx.quantScale.y);
    node.lower[2] = QuantiseLow(lower.z, ctx.quantScale.z);
    node.upper[0] = QuantiseHigh(upper.x, ctx.quantScale.x);
    node.upper[1] = QuantiseHigh(upper.y, ctx.quantScale.y);
    node.upper[2] = QuantiseHigh(upper.z, ctx.quantScale.z);
    node.packed = MeshNode::Pack(0, right - self);
    return self;
}

}  // namespace

std::vector<MeshNode> BuildTree(std::span<const Vec4> vertices,
                                std::vector<MeshTriangle>& triangles, const Vec4& quantScale,
                                const Vec4& offset) {
    // Adjacency first: an edge keyed by its two endpoints, matched against the same pair seen
    // the other way round. Anything shared by more than two faces is left open, because there is
    // no single neighbour for the internal-edge fix to name.
    struct EdgeRef {
        i32 triangle;
        i32 edge;
        i32 count;
    };
    std::map<std::pair<i32, i32>, EdgeRef> edges;
    for (i32 i = 0; i < static_cast<i32>(triangles.size()); ++i) {
        for (i32 k = 0; k < 3; ++k) {
            const i32 a = triangles[static_cast<usize>(i)].vertex[k];
            const i32 b = triangles[static_cast<usize>(i)].vertex[(k + 1) % 3];
            const auto key = std::minmax(a, b);
            auto [it, inserted] = edges.try_emplace(key, EdgeRef{i, k, 1});
            if (inserted) {
                continue;
            }
            ++it->second.count;
            if (it->second.count != 2) {
                continue;
            }
            const EdgeRef& other = it->second;
            // The neighbour's far corner: the vertex of the other triangle not on this edge.
            const MeshTriangle& mine = triangles[static_cast<usize>(i)];
            MeshTriangle& theirs = triangles[static_cast<usize>(other.triangle)];
            triangles[static_cast<usize>(i)].adjacent[k] = theirs.vertex[(other.edge + 2) % 3];
            theirs.adjacent[other.edge] = mine.vertex[(k + 2) % 3];
        }
    }
    for (auto& [key, ref] : edges) {
        if (ref.count <= 2) {
            continue;
        }
        // Shared by three or more: retract the link rather than pick one arbitrarily.
        triangles[static_cast<usize>(ref.triangle)].adjacent[ref.edge] = -1;
    }

    if (triangles.empty()) {
        return {};
    }

    BuildContext ctx;
    ctx.vertices = vertices;
    ctx.triangles = &triangles;
    ctx.quantScale = quantScale;
    ctx.offset = offset;
    ctx.order.resize(triangles.size());
    std::iota(ctx.order.begin(), ctx.order.end(), 0);

    BuildRange(ctx, 0, static_cast<i32>(triangles.size()));

    // Leaves index a contiguous run, so the triangles have to end up in the order the split
    // produced rather than the order they were authored in.
    std::vector<MeshTriangle> reordered;
    reordered.reserve(triangles.size());
    for (i32 index : ctx.order) {
        reordered.push_back(triangles[static_cast<usize>(index)]);
    }
    // No index needs rewriting alongside: adjacency names *vertices*, not triangles, so moving a
    // triangle does not invalidate anyone's neighbour.
    triangles = std::move(reordered);
    return ctx.nodes;
}

}  // namespace snowball
