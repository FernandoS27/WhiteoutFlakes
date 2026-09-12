#include "whiteout/flakes/types.h"
#include "whiteout/flakes/util/team_glow_data.h"

#include "team_glow_embedded.h"

#include <whiteout/textures/texture.h>
#include <whiteout/textures/tga/tga.h>

#include <algorithm>
#include <cmath>

namespace whiteout::flakes::io {
namespace {

/// The art's mean intensity per integer radius, in source pixels. Measured,
/// the glow is radial to within +-10/255 of this (a faint painted tilt), so
/// the profile carries the whole shape and the output stops being limited to
/// the art's 256 pixels and 117 levels.
std::vector<f32> RadialProfile(const u8* src, i32 w, i32 h) {
    const f32 cx = static_cast<f32>(w - 1) * 0.5f;
    const f32 cy = static_cast<f32>(h - 1) * 0.5f;
    const usize bins = static_cast<usize>(std::hypot(cx, cy)) + 2;
    std::vector<f32> mean(bins, 0.0f);
    std::vector<f32> count(bins, 0.0f);
    for (i32 y = 0; y < h; y++) {
        for (i32 x = 0; x < w; x++) {
            const f32 r = std::hypot(static_cast<f32>(x) - cx, static_cast<f32>(y) - cy);
            const usize bin = static_cast<usize>(r + 0.5f);
            if (bin >= bins)
                continue;
            // The shape is on the grey RGB, not the alpha, which is flat 255.
            mean[bin] += static_cast<f32>(src[(static_cast<usize>(y) * w + x) * 4]);
            count[bin] += 1.0f;
        }
    }
    for (usize i = 0; i < bins; i++)
        mean[i] = count[i] > 0.0f ? mean[i] / count[i] : 0.0f;
    // The centre bin catches no pixel on an even-sided source -- the nearest
    // four sit at r = 0.71 -- and an empty bin samples as a dark pinhole in
    // the middle of the glow. Carry the next ring inwards; the empty bins past
    // the corner radius carry zero, which is what they already hold.
    for (usize i = bins; i-- > 0;) {
        if (count[i] <= 0.0f)
            mean[i] = i + 1 < bins ? mean[i + 1] : 0.0f;
    }
    return mean;
}

/// A 4x4 ordered dither, each cell `(bayer + 0.5) / 16 - 0.5`.
///
/// The ramp spans about 107 of 255 levels over hundreds of pixels, so rounding
/// to 8 bits puts a ring every few pixels -- and the glow is multiplied by
/// `hdrEmissiveMultiplier` on the way to the screen, which scales those steps
/// up with it. Offsetting each pixel's rounding by under half a level trades
/// the rings for noise below one level, and a mip filter averages it back to
/// the exact value.
constexpr f32 kDither[16] = {
    -0.469f, 0.031f,  -0.344f, 0.156f,  //
    0.281f,  -0.219f, 0.406f,  -0.094f, //
    -0.281f, 0.219f,  -0.406f, 0.094f,  //
    0.469f,  -0.031f, 0.344f,  -0.156f,
};

u8 Quantize(f32 value, i32 x, i32 y) {
    const f32 dithered = value + 0.5f + kDither[(y & 3) * 4 + (x & 3)];
    return static_cast<u8>(std::clamp(dithered, 0.0f, 255.0f));
}

f32 SampleProfile(const std::vector<f32>& profile, f32 r) {
    const usize i = static_cast<usize>(r);
    if (i + 1 >= profile.size())
        return 0.0f;
    const f32 frac = r - static_cast<f32>(i);
    return profile[i] + (profile[i + 1] - profile[i]) * frac;
}

} // namespace

std::vector<u8> DecodeTeamGlow(u8 tcR, u8 tcG, u8 tcB, i32& outW, i32& outH, i32 size) {
    using namespace whiteout::textures;
    tga::Parser parser;
    auto tex = parser.parse(std::span<const u8>(kTeamGlowTGA, sizeof(kTeamGlowTGA)));
    if (tex) {
        tex->format(PixelFormat::RGBA8);
        auto& mip = tex->mipLevel(0);
        const i32 srcW = (i32)mip.width;
        const i32 srcH = (i32)mip.height;
        auto srcData = tex->data();
        const u8* src = srcData.data() + mip.offset;

        const std::vector<f32> profile = RadialProfile(src, srcW, srcH);
        const i32 side = size > 0 ? size : srcW;
        outW = side;
        outH = side;

        // Source radii per output pixel, so the disc keeps the fraction of
        // the frame it was painted at whatever the side.
        const f32 centre = static_cast<f32>(side - 1) * 0.5f;
        const f32 toSource = static_cast<f32>(srcW) / static_cast<f32>(side);

        std::vector<u8> result(static_cast<usize>(side) * side * 4);
        for (i32 y = 0; y < side; y++) {
            for (i32 x = 0; x < side; x++) {
                const f32 r = std::hypot(static_cast<f32>(x) - centre,
                                         static_cast<f32>(y) - centre) *
                              toSource;
                const f32 intensity = SampleProfile(profile, r) / 255.0f;
                const usize i = (static_cast<usize>(y) * side + x) * 4;
                // One offset for all three channels: a per-channel offset
                // would tint the grey ramp.
                result[i + 0] = Quantize(tcR * intensity, x, y);
                result[i + 1] = Quantize(tcG * intensity, x, y);
                result[i + 2] = Quantize(tcB * intensity, x, y);
                result[i + 3] = 255;
            }
        }
        return result;
    }

    outW = 4;
    outH = 4;
    std::vector<u8> fb(64);
    for (i32 i = 0; i < 16; i++) {
        fb[i * 4] = tcR;
        fb[i * 4 + 1] = tcG;
        fb[i * 4 + 2] = tcB;
        fb[i * 4 + 3] = 255;
    }
    return fb;
}

} // namespace whiteout::flakes::io
