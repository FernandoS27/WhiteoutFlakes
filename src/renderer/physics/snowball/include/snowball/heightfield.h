//===----------------------------------------------------------------------===//
// snowball/heightfield.h -- terrain as a regular grid of samples.
//
// A height field is a *source of triangles*, not a shape the narrowphase collides with: nothing
// tests against a height field directly. A query returns the cells an AABB overlaps, each cell
// yields two triangles, and those go through the ordinary triangle entry points. That is why
// this header produces `Triangle` and stops.
//
// **The grid lies in XY with height along Z**, while every body in the engine falls along -Y.
// That mismatch is deliberate, not a bug to align: it is the frame terrain data is authored
// in, and part of the asset contract. A host has to rotate its fields into the world, and one
// that forgets will get a wall instead of a floor.
//
// Samples are **six bytes**: a height, a material, and a flags word whose lowest bit marks the
// cell as a hole. Packing them as four would halve the memory and read every second sample
// wrong; treating the third field as padding silently fills in every hole in the terrain.
//===----------------------------------------------------------------------===//
#pragma once

#include <span>
#include <vector>

#include "snowball/common_types.h"
#include "snowball/transform.h"
#include "snowball/triangle.h"

namespace snowball {

/// One grid sample as it is stored: `int16` height, `int16` material, and a flags field of
/// which exactly one bit is meaningful.
struct HeightSample {
    i16 height{0};
    i16 material{0};
    i16 flags{0};

    /// Bit 0 set means the cell this sample anchors is a hole and emits no triangles.
    bool IsHole() const { return (flags & 1) != 0; }
};

struct HeightField {
    i32 countX{0};              ///< rows; the x axis, and the SLOWER of the two
    i32 countY{0};              ///< columns; the y axis, and the sample array's row stride
    f32 gridScale{1.0f};        ///< spacing along both x and y
    f32 heightBase{0.0f};       ///< height = sample*heightScale + heightBase
    f32 heightScale{1.0f};
    std::span<const HeightSample> samples;

    /// Two triangles per cell, over a `(countX-1) x (countY-1)` grid of cells. The expression
    /// is only meaningful while both counts are at least two -- exactly what `IsValid` checks
    /// -- and below that it can come back negative, so validate before trusting it.
    i32 TriangleCount() const { return (2 * countX - 2) * (countY - 1); }

    bool IsValid() const { return countX >= 2 && countY >= 2; }

    f32 HeightAt(i32 row, i32 column) const {
        return static_cast<f32>(samples[static_cast<usize>(row * countY + column)].height) *
                   heightScale +
               heightBase;
    }

    u16 MaterialAt(i32 row, i32 column) const {
        return static_cast<u16>(samples[static_cast<usize>(row * countY + column)].material);
    }

    bool IsHoleAt(i32 row, i32 column) const {
        return samples[static_cast<usize>(row * countY + column)].IsHole();
    }
};

/// @brief The `index`-th triangle, in the field's own frame.
///
/// Cells are numbered row-major and each contributes two triangles, so `index >> 1` selects the
/// cell and the low bit selects which half. **Which diagonal splits a cell alternates on a
/// checkerboard** -- `(row ^ column) & 1` -- so that a run of cells does not develop a visible
/// grain along one diagonal. The parity is contractual: getting it backwards leaves every
/// triangle geometrically plausible and half of them on the wrong side of the surface.
Triangle HeightFieldTriangle(const HeightField& field, i32 index);

/// @brief The same triangle, plus the neighbours the internal-edge fix needs.
///
/// Every edge of a height field has a neighbour, so all three `hasAdjacent` come back set even
/// at the border -- the field invents one by **clamping the height sample but not the position**.
/// The phantom vertex sits outside the grid at the edge's own height, which makes the boundary
/// behave as though the terrain continued flat rather than dropping away. The `adjacentId`s
/// stay unclamped, which is the only way to tell the two apart at a border.
Triangle HeightFieldTriangleFull(const HeightField& field, i32 index);

/// @brief The material of the cell `index` falls in. `-1` when the index is out of range.
///
/// Widened to `int` only to carry that sentinel: materials are zero-extended `uint16` values,
/// so ones above 32767 are ordinary materials rather than negatives or errors.
i32 HeightFieldMaterial(const HeightField& field, i32 index);

/// @brief Append the indices of every triangle whose cell overlaps `bounds` in x and y.
///
/// **The height is never tested.** The query is purely two-dimensional: a body a mile above the
/// terrain still collects every cell beneath it, and the narrowphase is left to reject them.
/// Both triangles of a cell are emitted together, and cells flagged as holes are skipped
/// entirely. Out-of-range cells are dropped rather than clamped, so a query off the edge of the
/// field is empty rather than a wall of border triangles.
void HeightFieldQuery(const HeightField& field, const Aabb& bounds, std::vector<i32>& out);

/// @brief World bounds of the whole field under `xf`, expanded by `radius`.
///
/// Every sample is transformed -- there is no cached bound and no shortcut through the eight
/// corners, because a rotated height field's extremes are interior samples. A field with no
/// columns yields the inverted bounds this starts from -- deliberately unguarded, which is why
/// `IsValid` exists for callers to check first.
Aabb HeightFieldAabb(const HeightField& field, const Transform& xf, f32 radius = 0.0f);

}  // namespace snowball
