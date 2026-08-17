#include "snowball/raycast.h"

#include <algorithm>
#include <cmath>

#include "snowball/math.h"

namespace snowball {
namespace {

Vec4 Normalize(const Vec4& v) { return v * Rsqrt(Vec4::Splat(LengthSquared3(v))).x; }

/// Into a shape's own frame: undo the rotation, then the translation.
Vec4 ToLocal(const Transform& xf, const Vec4& p) {
    return Transpose3(RotationMatrix(xf.rotation)).Transform(p - xf.position);
}

/// One axis of the grid walk. `step` is zero when the ray does not cross a boundary on this
/// axis at all, and `next` is then infinite so the other axis always wins.
struct Walk {
    i32 cell{0};
    i32 last{0};
    i32 step{0};
    f32 next{constants::kMaxFloat.x};
    f32 delta{0.0f};
};

Walk SetUpWalk(f32 from, f32 to, f32 gridScale) {
    Walk w;
    w.cell = static_cast<i32>(std::floor(from / gridScale));
    w.last = static_cast<i32>(std::floor(to / gridScale));
    if (w.cell == w.last) {
        return w;  // never leaves this row or column
    }
    const f32 inverse = 1.0f / std::abs(to - from);
    const f32 low = static_cast<f32>(w.cell) * gridScale;
    w.step = w.cell < w.last ? 1 : -1;
    w.next = (w.step > 0 ? (low + gridScale) - from : from - low) * inverse;
    w.delta = gridScale * inverse;
    return w;
}

}  // namespace

bool TriangleRayCast(const Triangle& t, const Vec4& p1, const Vec4& p2, f32 maxFraction,
                     RayCastOutput& out) {
    const Vec4 d = p2 - p1;
    const Vec4 e1 = t.v1 - p1;
    const Vec4 e2 = t.v2 - p1;
    const Vec4 e3 = t.v3 - p1;

    // Containment first, and it costs no division: the segment passes through the triangle only
    // if all three tetrahedra it makes with the edges have the same orientation.
    const f32 w1 = Dot3(Cross3(e1, e3), d);
    const f32 w2 = Dot3(Cross3(e3, e2), d);
    const f32 w3 = Dot3(Cross3(e2, e1), d);
    if (std::min(w3, std::min(w2, w1)) < 0.0f) {
        return false;
    }

    const Vec4 normal = Cross3(t.v2 - t.v1, t.v3 - t.v1);
    // One-sided: a ray travelling *with* the normal leaves through the back and is not a hit.
    const f32 denominator = -Dot3(d, normal);
    if (denominator < constants::kEpsilon.x) {
        return false;
    }
    const f32 numerator = -Dot3(e1, normal);
    if (numerator < 0.0f || numerator > maxFraction * denominator) {
        return false;
    }
    out.fraction = numerator / denominator;
    out.normal = Normalize(normal);
    return true;
}

bool HeightFieldRayCast(const HeightField& field, const RayCastInput& input, const Transform& xf,
                        RayCastOutput& out) {
    if (!field.IsValid()) {
        return false;
    }
    const Vec4 from = ToLocal(xf, input.p1);
    const Vec4 to = ToLocal(xf, input.p2);
    Walk row = SetUpWalk(from.x, to.x, field.gridScale);
    Walk column = SetUpWalk(from.y, to.y, field.gridScale);

    f32 maxFraction = input.maxFraction;
    bool hit = false;
    const Mtx rotation = RotationMatrix(xf.rotation);

    for (;;) {
        if (row.cell >= 0 && row.cell < field.countX - 1 && column.cell >= 0 &&
            column.cell < field.countY - 1 && !field.IsHoleAt(row.cell, column.cell)) {
            const i32 base = 2 * (row.cell * (field.countY - 1) + column.cell);
            for (i32 half = 0; half < 2; ++half) {
                RayCastOutput local;
                if (!TriangleRayCast(HeightFieldTriangle(field, base + half), from, to,
                                     maxFraction, local)) {
                    continue;
                }
                // Shorten as we go, so a later cell can only improve on this.
                maxFraction = local.fraction;
                out.fraction = local.fraction;
                out.normal = rotation.Transform(local.normal);
                out.triangle = base + half;
                out.material = field.MaterialAt(row.cell, column.cell);
                hit = true;
            }
        }
        // The 0.01 slack is deliberate: a cell whose entry parameter is only a hundredth
        // past the ray's end is still visited, so a hit exactly at the endpoint is not lost.
        if (row.next > maxFraction + 0.01f && column.next > maxFraction + 0.01f) {
            break;
        }
        if (row.next > column.next) {
            if (column.cell == column.last) {
                break;
            }
            column.cell += column.step;
            column.next += column.delta;
        } else {
            if (row.cell == row.last) {
                break;
            }
            row.cell += row.step;
            row.next += row.delta;
        }
    }
    return hit;
}

}  // namespace snowball
