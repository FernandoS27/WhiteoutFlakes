#pragma once

/// @file anim_math.h
/// @brief Animation kernels shared across formats.
///
/// The bar for living here is deliberately high: a kernel qualifies when two
/// or more format adapters need arithmetic that is *identical*, not merely
/// similar. Most of what looks shared between the MDX, M2 and M3 samplers is
/// not — and folding it together would either change behaviour or reproduce
/// the same branches behind a mode flag. What was examined and left alone:
///
///  - **Quaternion interpolation.** Warcraft III slerps with a shortest-arc
///    sign flip, a 0.9 dot threshold and a renormalise; World of Warcraft
///    nlerps with *no* sign flip (`C4Quaternion::Nlerp`, and flipping would
///    produce a different rotation wherever a key pair straddles hemispheres);
///    StarCraft II lerps raw at track level with neither, and slerps only when
///    combining layers. Three engines, three answers, all load-bearing.
///  - **Key bracketing.** The search is a shared `upper_bound`, but the policy
///    around it is the format: MDX interpolates across the loop seam within a
///    sequence window, M2 holds at sub-array edges, M3 wraps on each track's
///    own duration. The policy is the interesting part and it is all different.
///  - **Hermite / Bezier.** MDX is the only consumer — M2 leaves splines to
///    cameras it does not sample, and M3 has no spline tracks.
///  - **Parent-inheritance strips.** MDX and M2 each have their own, with
///    different pivot re-anchoring; M3 turned out to have none at all (its
///    `Inherit*` bone flags are authoring metadata the runtime never reads),
///    so what looked like a three-way merge is a two-way one between two
///    implementations that genuinely differ.

#include "whiteout/flakes/types.h"

namespace whiteout::flakes::renderer::animation {

/// @brief Local bone matrix for a format that stores a pivot separately from
///        the animated translation.
///
/// Composes `T(-pivot) · S · R · T(pivot + t)`, i.e.
/// `p' = ((p - pivot) · S · R) + pivot + t`.
///
/// This is what World of Warcraft's `AnimateMT` builds — `M = R;
/// M.Scale(s); M.row3 += pivot + t; M.Translate(-pivot)`, where
/// `C44Matrix::Translate` and `::Scale` both pre-multiply — and Warcraft III's
/// node transform is the same expression. The two had separate copies only
/// because the MDX one took `mdx::` types and pulled that format's structure
/// header in behind it.
///
/// StarCraft II does not use this: an M3 bone's sampled TRS is already a
/// complete local transform with no pivot to compose around.
inline Matrix44f ComposePivotSRT(const Vector3f& t, const Quaternion& r, const Vector3f& s,
                                 const Vector3f& pivot) {
    const Matrix44f mS = Matrix44f::scaling(s);
    const Matrix44f mR = Matrix44f::rotation(r).transpose();
    const Matrix44f mNegPivot = Matrix44f::translation({-pivot.x, -pivot.y, -pivot.z});
    const Matrix44f mPivotPlusT =
        Matrix44f::translation({pivot.x + t.x, pivot.y + t.y, pivot.z + t.z});
    return mNegPivot * mS * mR * mPivotPlusT;
}

} // namespace whiteout::flakes::renderer::animation
