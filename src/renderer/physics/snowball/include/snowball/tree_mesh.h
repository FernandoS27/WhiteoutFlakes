//===----------------------------------------------------------------------===//
// snowball/tree_mesh.h -- an arbitrary triangle mesh behind a baked BVH.
//
// The other half of world collision. Where a height field is a grid whose triangles are
// arithmetic, this is an authored mesh: explicit vertices, explicit topology, and a bounding
// volume hierarchy to find the few triangles a query touches.
//
// **The tree is baked, not built.** `Create` adopts three spans out of the asset and computes
// nothing, because the hierarchy is produced offline by the content pipeline. The node
// encoding is therefore a fixed asset format the traversal must consume exactly as stored,
// while `BuildTree` is a convenience for meshes that arrive without a tree and is free to
// choose its strategy. A host that has its own baked trees should feed them straight in.
//
// Nodes are 16 bytes and quantised: six `int16` bounds and a packed word. Queries therefore run
// in the mesh's own quantised space, and the query AABB is brought *down* into it rather than
// the nodes being brought up -- one transform for the whole descent instead of one per node.
//===----------------------------------------------------------------------===//
#pragma once

#include <span>
#include <vector>

#include "snowball/common_types.h"
#include "snowball/raycast.h"
#include "snowball/transform.h"
#include "snowball/triangle.h"

namespace snowball {

/// One node of the baked hierarchy, exactly as the asset stores it.
///
/// `payload` means different things by node kind, which is why it is one field: on a leaf it is
/// the first triangle index, on an interior node it is the distance **in nodes** to the right
/// child. The left child is always the next node, so it needs no storage.
struct MeshNode {
    i16 lower[3]{};
    i16 upper[3]{};
    u32 packed{0};

    /// Triangles in this leaf; zero means an interior node. Four bits, so at most 15.
    i32 TriangleCount() const { return static_cast<i32>((packed >> 2) & 0xF); }
    i32 Payload() const { return static_cast<i32>(packed >> 6); }
    bool IsLeaf() const { return TriangleCount() != 0; }

    static u32 Pack(i32 triangleCount, i32 payload) {
        return (static_cast<u32>(payload) << 6) | ((static_cast<u32>(triangleCount) & 0xF) << 2);
    }
};

/// A triangle's topology: three corners, and the far corner of each neighbour. `-1` marks an
/// open edge, and unlike a height field a mesh really can have them -- which is what makes
/// `hasAdjacent` a variable rather than a constant on this side.
struct MeshTriangle {
    i32 vertex[3]{-1, -1, -1};
    i32 adjacent[3]{-1, -1, -1};
    u16 material{0};

    /// A **ray filter mask**, ANDed with `RayCastInput::mask`; a triangle sharing no bit with
    /// the ray is skipped. Nothing on the contact path reads it, and height fields have no
    /// equivalent -- the ray cast is this field's only consumer.
    u16 mask{0};
};

/// The asset side: everything a mesh needs, none of it owned.
struct MeshData {
    std::span<const MeshNode> nodes;
    std::span<const Vec4> vertices;
    std::span<const MeshTriangle> triangles;

    /// Added to a vertex *before* scaling; the query maps the other way, so the two must stay
    /// exact inverses or a query returns triangles that are not there.
    Vec4 offset{};

    /// Multiplies a node's quantised bounds back into vertex space.
    Vec4 quantScale{constants::kOne};

    /// The traversal stack is a fixed 128 entries, and this baked figure is what gets checked
    /// against that limit -- the descent never tracks its own depth.
    i32 treeDepth{0};
};

class TreeMesh {
public:
    /// @brief Adopt a baked mesh at `scale`. Computes only the reciprocal.
    ///
    /// The inverse scale goes through the engine's `Rcp` -- estimate plus one Newton step,
    /// masked to zero on a zero component -- so a mesh created at scale s and one created at 1
    /// then scaled by s do **not** hold the same inverse. `ApplyScale` divides exactly, and the
    /// disagreement is deliberate: unifying the two would change query results at the bit
    /// level, which the engine's determinism contract forbids.
    void Create(const MeshData& data, const Vec4& scale);

    /// @brief Rescale in place. Divides exactly; see `Create`.
    void ApplyScale(const Vec4& scale);

    i32 TriangleCount() const { return triangleCount_; }

    /// @brief The `index`-th triangle in world-local space: `(vertex + offset) * scale`.
    Triangle GetTriangle(i32 index) const;

    /// @brief The same, plus adjacency, identity and material.
    Triangle GetTriangleFull(i32 index) const;

    /// @brief Append every triangle whose bounds overlap `bounds`.
    ///
    /// Two filters, not one: the descent rejects whole subtrees on their quantised bounds, and
    /// then each triangle of a surviving leaf is tested again against its *exact* corners. The
    /// second pass is what keeps quantisation from leaking false positives out to the
    /// narrowphase.
    void Query(const Aabb& bounds, std::vector<i32>& out) const;

    /// @brief The nearest triangle a ray hits, or false.
    ///
    /// Descends the same hierarchy `Query` does, with two differences that matter. Each node is
    /// rejected by a segment/box separating-axis test rather than by bounds overlap alone, which
    /// is far tighter for a long thin ray. And **bits 0..1 of a node's packed word name its split
    /// axis** -- the only use those bits have -- so the child on the ray's own side of the split
    /// is visited first and the far one is usually rejected outright by the shortened
    /// `maxFraction`.
    bool RayCast(const RayCastInput& input, const Transform& xf, RayCastOutput& out) const;

    /// @brief World bounds over every vertex, expanded by `radius`.
    ///
    /// A mesh with no vertices reports the **unit cube**, not inverted bounds. The asymmetry
    /// with a height field in the same situation matches the reference engine; do not align
    /// the two.
    Aabb GetAabb(const Transform& xf, f32 radius = 0.0f) const;

private:
    MeshData data_{};
    Vec4 scale_{constants::kOne};
    Vec4 inverseScale_{constants::kOne};
    i32 triangleCount_{0};
};

/// @brief Build a hierarchy over `triangles`, and fill in their adjacency.
///
/// Only the node encoding is contractual (see the header note); the strategy is a free
/// choice: median split on the widest axis, leaves of at most `kMaxLeafTriangles`, which is
/// the encoding's own ceiling.
///
/// Adjacency is derived by matching each edge against its reverse: an edge shared by exactly two
/// triangles links them, and anything else is left open. That deliberately refuses to link an
/// edge shared by three or more faces, where there is no single neighbour to name.
std::vector<MeshNode> BuildTree(std::span<const Vec4> vertices,
                                std::vector<MeshTriangle>& triangles, const Vec4& quantScale,
                                const Vec4& offset);

/// The four bits the node encoding gives a leaf's triangle count.
inline constexpr i32 kMaxLeafTriangles = 15;

}  // namespace snowball
