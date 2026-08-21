#include "renderer/assets/env_cube_texture.h"

#include <algorithm>
#include <cmath>

namespace whiteout::flakes::renderer::assets {
namespace {

/// Bilinear RGBA8 fetch with clamped edges. The disc a sphere map actually
/// covers is inscribed in the image, so the clamp only ever sees the corners
/// no direction reaches.
void SampleBilinear(std::span<const u8> src, i32 w, i32 h, f32 u, f32 v, u8 out[4]) {
    const f32 x = std::clamp(u, 0.0f, 1.0f) * static_cast<f32>(w) - 0.5f;
    const f32 y = std::clamp(v, 0.0f, 1.0f) * static_cast<f32>(h) - 0.5f;
    const i32 x0 = static_cast<i32>(std::floor(x));
    const i32 y0 = static_cast<i32>(std::floor(y));
    const f32 fx = x - static_cast<f32>(x0);
    const f32 fy = y - static_cast<f32>(y0);
    const i32 xa = std::clamp(x0, 0, w - 1), xb = std::clamp(x0 + 1, 0, w - 1);
    const i32 ya = std::clamp(y0, 0, h - 1), yb = std::clamp(y0 + 1, 0, h - 1);
    for (i32 c = 0; c < 4; ++c) {
        const f32 t00 = src[(static_cast<usize>(ya) * w + xa) * 4 + c];
        const f32 t10 = src[(static_cast<usize>(ya) * w + xb) * 4 + c];
        const f32 t01 = src[(static_cast<usize>(yb) * w + xa) * 4 + c];
        const f32 t11 = src[(static_cast<usize>(yb) * w + xb) * 4 + c];
        const f32 top = t00 + (t10 - t00) * fx;
        const f32 bot = t01 + (t11 - t01) * fx;
        out[c] = static_cast<u8>(std::lround(std::clamp(top + (bot - top) * fy, 0.0f, 255.0f)));
    }
}

/// Direction for texel (@p s, @p t) in [-1,1] on cube @p face, DDS/D3D face
/// order and orientation. Not normalised — the caller does that.
Vector3f FaceDirection(i32 face, f32 s, f32 t) {
    switch (face) {
    case 0: return {1.0f, -t, -s};  // +X
    case 1: return {-1.0f, -t, s};  // -X
    case 2: return {s, 1.0f, t};    // +Y
    case 3: return {s, -1.0f, -t};  // -Y
    case 4: return {s, -t, 1.0f};   // +Z
    default: return {-s, -t, -1.0f}; // -Z
    }
}

} // namespace

i32 SphereMapCubeFaceSize(i32 srcW, i32 srcH) {
    const i32 edge = std::max(srcW, srcH);
    i32 size = 16;
    while (size * 2 <= edge && size < 512)
        size *= 2;
    return size;
}

bool BuildCubeFromSphereMap(std::span<const u8> srcRgba8, i32 srcW, i32 srcH, i32 faceSize,
                            std::vector<u8>& outBytes, i32& outMipLevels) {
    if (srcW <= 0 || srcH <= 0 || faceSize <= 0 ||
        srcRgba8.size() < static_cast<usize>(srcW) * srcH * 4)
        return false;

    i32 mips = 1;
    for (i32 s = faceSize; s > 1; s /= 2)
        ++mips;
    outMipLevels = mips;

    usize total = 0;
    for (i32 m = 0; m < mips; ++m) {
        const usize s = static_cast<usize>(std::max(1, faceSize >> m));
        total += s * s * 4;
    }
    outBytes.assign(total * 6, 0);

    u8* cursor = outBytes.data();
    std::vector<u8> prev, cur;
    for (i32 face = 0; face < 6; ++face) {
        // Mip 0 by projection, the rest by a 2x2 box off the level above —
        // re-projecting each level would alias, since the source is only
        // prefiltered for its own resolution.
        cur.assign(static_cast<usize>(faceSize) * faceSize * 4, 0);
        for (i32 y = 0; y < faceSize; ++y) {
            const f32 t = (2.0f * (static_cast<f32>(y) + 0.5f) / static_cast<f32>(faceSize)) - 1.0f;
            for (i32 x = 0; x < faceSize; ++x) {
                const f32 s =
                    (2.0f * (static_cast<f32>(x) + 0.5f) / static_cast<f32>(faceSize)) - 1.0f;
                Vector3f d = FaceDirection(face, s, t);
                const f32 len = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
                d = {d.x / len, d.y / len, d.z / len};
                SampleBilinear(srcRgba8, srcW, srcH, d.x * 0.5f + 0.5f, d.z * -0.5f + 0.5f,
                               &cur[(static_cast<usize>(y) * faceSize + x) * 4]);
            }
        }
        std::copy(cur.begin(), cur.end(), cursor);
        cursor += cur.size();

        i32 w = faceSize;
        for (i32 m = 1; m < mips; ++m) {
            prev.swap(cur);
            const i32 hw = std::max(1, w / 2);
            cur.assign(static_cast<usize>(hw) * hw * 4, 0);
            for (i32 y = 0; y < hw; ++y) {
                for (i32 x = 0; x < hw; ++x) {
                    for (i32 c = 0; c < 4; ++c) {
                        const i32 x0 = std::min(x * 2, w - 1), x1 = std::min(x * 2 + 1, w - 1);
                        const i32 y0 = std::min(y * 2, w - 1), y1 = std::min(y * 2 + 1, w - 1);
                        const u32 sum = prev[(static_cast<usize>(y0) * w + x0) * 4 + c] +
                                        prev[(static_cast<usize>(y0) * w + x1) * 4 + c] +
                                        prev[(static_cast<usize>(y1) * w + x0) * 4 + c] +
                                        prev[(static_cast<usize>(y1) * w + x1) * 4 + c];
                        cur[(static_cast<usize>(y) * hw + x) * 4 + c] = static_cast<u8>(sum / 4);
                    }
                }
            }
            std::copy(cur.begin(), cur.end(), cursor);
            cursor += cur.size();
            w = hw;
        }
    }
    return true;
}

} // namespace whiteout::flakes::renderer::assets
