#include "snowball/heightfield.h"

#include <algorithm>
#include <cmath>

namespace snowball {
namespace {

/// Which cell a triangle index falls in, and which half of it.
struct Cell {
    i32 row{0};
    i32 column{0};
    bool second{false};
};

Cell CellOf(const HeightField& field, i32 index) {
    const i32 cell = index >> 1;
    // countY is the *column* count, so `countY - 1` cells fit across a row. Dividing by countX
    // instead transposes the whole field and still produces a plausible surface.
    return Cell{cell / (field.countY - 1), cell % (field.countY - 1), (index & 1) != 0};
}

Vec4 Corner(const HeightField& field, i32 row, i32 column) {
    return Vec4{static_cast<f32>(row) * field.gridScale,
                static_cast<f32>(column) * field.gridScale, field.HeightAt(row, column), 0.0f};
}

/// A neighbour vertex: positioned where the lattice says, but sampled from the nearest cell that
/// exists. The two disagree only outside the grid, which is exactly the border case.
Vec4 Phantom(const HeightField& field, i32 row, i32 column) {
    const i32 sampleRow = std::clamp(row, 0, field.countX - 1);
    const i32 sampleColumn = std::clamp(column, 0, field.countY - 1);
    return Vec4{static_cast<f32>(row) * field.gridScale,
                static_cast<f32>(column) * field.gridScale,
                field.HeightAt(sampleRow, sampleColumn), 0.0f};
}

/// Vertex identity in the lattice, offset by one row and one column so that the phantom
/// neighbours off the low edge stay non-negative and distinct.
i32 VertexId(const HeightField& field, i32 row, i32 column) {
    return (row + 1) * field.countY + column + 1;
}

bool InRange(const HeightField& field, i32 index) {
    return field.IsValid() && index >= 0 && index < field.TriangleCount();
}

}  // namespace

Triangle HeightFieldTriangle(const HeightField& field, i32 index) {
    Triangle t;
    if (!InRange(field, index)) {
        return t;
    }
    const Cell c = CellOf(field, index);
    const Vec4 a = Corner(field, c.row, c.column);
    const Vec4 b = Corner(field, c.row + 1, c.column);
    const Vec4 d = Corner(field, c.row + 1, c.column + 1);
    const Vec4 e = Corner(field, c.row, c.column + 1);

    if (((c.row ^ c.column) & 1) == 0) {
        // Split a-d: the diagonal runs from the cell's low corner to its high one.
        t.v1 = a;
        t.v2 = c.second ? d : b;
        t.v3 = c.second ? e : d;
    } else {
        // Split b-e, the other way, so neighbouring cells never share a diagonal direction.
        t.v1 = c.second ? b : a;
        t.v2 = c.second ? d : b;
        t.v3 = e;
    }
    return t;
}

Triangle HeightFieldTriangleFull(const HeightField& field, i32 index) {
    Triangle t = HeightFieldTriangle(field, index);
    if (!InRange(field, index)) {
        return t;
    }
    const Cell c = CellOf(field, index);
    const i32 r = c.row;
    const i32 k = c.column;

    // Edge k runs from vertex k to vertex k+1, and its neighbour is either the cell's other half
    // (across the shared diagonal) or the far corner of the cell over that edge.
    if (((r ^ k) & 1) == 0) {
        if (!c.second) {                                          // (a, b, d)
            t.adjacent = {Phantom(field, r + 1, k - 1), Phantom(field, r + 2, k),
                          Corner(field, r, k + 1)};
            t.adjacentId = {VertexId(field, r + 1, k - 1), VertexId(field, r + 2, k),
                            VertexId(field, r, k + 1)};
            t.vertexId = {VertexId(field, r, k), VertexId(field, r + 1, k),
                          VertexId(field, r + 1, k + 1)};
        } else {                                                  // (a, d, e)
            t.adjacent = {Corner(field, r + 1, k), Phantom(field, r, k + 2),
                          Phantom(field, r - 1, k + 1)};
            t.adjacentId = {VertexId(field, r + 1, k), VertexId(field, r, k + 2),
                            VertexId(field, r - 1, k + 1)};
            t.vertexId = {VertexId(field, r, k), VertexId(field, r + 1, k + 1),
                          VertexId(field, r, k + 1)};
        }
    } else {
        if (!c.second) {                                          // (a, b, e)
            t.adjacent = {Phantom(field, r, k - 1), Corner(field, r + 1, k + 1),
                          Phantom(field, r - 1, k)};
            t.adjacentId = {VertexId(field, r, k - 1), VertexId(field, r + 1, k + 1),
                            VertexId(field, r - 1, k)};
            t.vertexId = {VertexId(field, r, k), VertexId(field, r + 1, k),
                          VertexId(field, r, k + 1)};
        } else {                                                  // (b, d, e)
            t.adjacent = {Phantom(field, r + 2, k + 1), Phantom(field, r + 1, k + 2),
                          Corner(field, r, k)};
            t.adjacentId = {VertexId(field, r + 2, k + 1), VertexId(field, r + 1, k + 2),
                            VertexId(field, r, k)};
            t.vertexId = {VertexId(field, r + 1, k), VertexId(field, r + 1, k + 1),
                          VertexId(field, r, k + 1)};
        }
    }
    // Unconditional: a height field always answers with three neighbours, inventing them at the
    // border rather than reporting an open edge.
    t.hasAdjacent = {true, true, true};
    t.index = index;
    t.material = field.MaterialAt(r, k);
    return t;
}

i32 HeightFieldMaterial(const HeightField& field, i32 index) {
    if (!InRange(field, index)) {
        return -1;
    }
    const Cell c = CellOf(field, index);
    // Both halves of a cell carry the cell's material, taken from its low corner's sample.
    return field.MaterialAt(c.row, c.column);
}

void HeightFieldQuery(const HeightField& field, const Aabb& bounds, std::vector<i32>& out) {
    if (!field.IsValid()) {
        return;
    }
    const f32 inverse = 1.0f / field.gridScale;
    const i32 rowLo = static_cast<i32>(std::floor(bounds.lower.x * inverse));
    const i32 rowHi = static_cast<i32>(std::floor(bounds.upper.x * inverse));
    const i32 columnLo = static_cast<i32>(std::floor(bounds.lower.y * inverse));
    const i32 columnHi = static_cast<i32>(std::floor(bounds.upper.y * inverse));

    for (i32 row = rowLo; row <= rowHi; ++row) {
        if (row < 0 || row > field.countX - 2) {
            continue;
        }
        for (i32 column = columnLo; column <= columnHi; ++column) {
            if (column < 0 || column > field.countY - 2 || field.IsHoleAt(row, column)) {
                continue;
            }
            const i32 base = 2 * (row * (field.countY - 1) + column);
            out.push_back(base);
            out.push_back(base + 1);
        }
    }
}

Aabb HeightFieldAabb(const HeightField& field, const Transform& xf, f32 radius) {
    Aabb bounds{constants::kMaxFloat, -constants::kMaxFloat};
    const Mtx rotation = RotationMatrix(xf.rotation);
    for (i32 row = 0; row < field.countX; ++row) {
        for (i32 column = 0; column < field.countY; ++column) {
            const Vec4 p = rotation.Transform(Corner(field, row, column)) + xf.position;
            bounds.lower = {std::min(bounds.lower.x, p.x), std::min(bounds.lower.y, p.y),
                            std::min(bounds.lower.z, p.z), 0.0f};
            bounds.upper = {std::max(bounds.upper.x, p.x), std::max(bounds.upper.y, p.y),
                            std::max(bounds.upper.z, p.z), 0.0f};
        }
    }
    bounds.lower = bounds.lower - Vec4::Splat(radius);
    bounds.upper = bounds.upper + Vec4::Splat(radius);
    return bounds;
}

}  // namespace snowball
