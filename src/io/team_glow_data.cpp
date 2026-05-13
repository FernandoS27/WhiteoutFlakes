#include "whiteout/flakes/types.h"
#include "whiteout/flakes/util/team_glow_data.h"

#include "team_glow_embedded.h"

#include <whiteout/textures/texture.h>
#include <whiteout/textures/tga/tga.h>

namespace whiteout::flakes::io {

std::vector<u8> DecodeTeamGlow(u8 tcR, u8 tcG, u8 tcB, i32& outW, i32& outH) {
    using namespace whiteout::textures;
    tga::Parser parser;
    auto tex = parser.parse(std::span<const u8>(kTeamGlowTGA, sizeof(kTeamGlowTGA)));
    if (tex) {
        tex->format(PixelFormat::RGBA8);
        auto& mip = tex->mipLevel(0);
        outW = (i32)mip.width;
        outH = (i32)mip.height;
        auto srcData = tex->data();
        const u8* src = srcData.data() + mip.offset;
        i32 pixelCount = outW * outH;
        std::vector<u8> result(pixelCount * 4);
        for (i32 i = 0; i < pixelCount; i++) {

            f32 intensity = src[i * 4] / 255.0f;
            result[i * 4 + 0] = (u8)(tcR * intensity);
            result[i * 4 + 1] = (u8)(tcG * intensity);
            result[i * 4 + 2] = (u8)(tcB * intensity);
            result[i * 4 + 3] = src[i * 4 + 3];
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
