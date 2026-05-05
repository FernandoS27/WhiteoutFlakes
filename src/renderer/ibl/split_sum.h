#pragma once

#include "common_types.h"
#include "gfx/gfx.h"
#include <vector>

namespace WhiteoutDex::ibl {

constexpr i32 kSplitSumSize    = 128;
constexpr i32 kSplitSumSamples = 128;

void GenerateSplitSumLut(i32 size, i32 sampleCount,
                         std::vector<u8>& outPixels);

gfx::TextureHandle CreateSplitSumLutTexture(gfx::IGFXDevice& gfx);

}
