#pragma once

// ============================================================================
// Environment-map cube construction.
//
// StarCraft II authors reflection maps two ways: a real cube DDS behind the
// CubicEnvio mappings, and a flat 2D map behind the Spherical ones. The
// shader wants one lookup, so the 2D form is projected into a cube here at
// decode time. See BuildCubeFromSphereMap for why that projection is exact
// rather than a fit.
// ============================================================================

#include "whiteout/flakes/types.h"

#include <span>
#include <vector>

namespace whiteout::flakes::renderer::assets {

/// @brief Project a 2D spherical environment map into a 6-face RGBA8 cube
///        with a full mip chain, laid out layer-major / mip-minor the way
///        IGFXDevice::CreateTexture wants it.
///
/// The projection is retail's own, inverted. psuvmapping.fx GenSphericalEnvio
/// is `uv = lookup.xz * (0.5, -0.5) + 0.5` — it uses only x and z, so every
/// direction has one defined texel and building the cube costs no information
/// the lookup could have used. (It is many-to-one the other way: two
/// directions differing only in y share a texel, which is exactly why these
/// maps read soft near the horizon in the game too.)
///
/// Faces come out in DDS/D3D order (+X, -X, +Y, -Y, +Z, -Z) around the same
/// z-up vector space the `.m3` normals live in.
///
/// The resample runs in whatever space the source stores — no de-gamma. That
/// is deliberate: the alternative to this projection is the hardware sampling
/// the flat map directly, and it would filter in that same space.
///
/// @param srcRgba8 Tightly packed RGBA8, @p srcW * @p srcH * 4 bytes.
/// @param faceSize Edge length of each cube face; must be a power of two.
/// @returns false when the inputs do not describe an image.
bool BuildCubeFromSphereMap(std::span<const u8> srcRgba8, i32 srcW, i32 srcH, i32 faceSize,
                            std::vector<u8>& outBytes, i32& outMipLevels);

/// @brief A sensible cube face size for a source of @p srcW x @p srcH.
///
/// The source's longer edge, rounded DOWN to a power of two, floored at 16 and
/// capped at 512. Matching the edge is the closest of the powers of two: a
/// sphere map's whole azimuth range spans the disc's circumference, about
/// `pi * W` texels, while a cube's equator spans `4 * N` — so `N = W` slightly
/// over-samples (4/pi) and `N = W/2` would under-sample by more than half.
/// The cap is a memory bound; the faces are uncompressed RGBA8, and 512 is
/// already the largest spherical map the corpus ships.
i32 SphereMapCubeFaceSize(i32 srcW, i32 srcH);

} // namespace whiteout::flakes::renderer::assets
