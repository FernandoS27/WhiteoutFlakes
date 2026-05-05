#pragma once

#include "common_types.h"
#include "gfx/gfx_types.h"

#include <string_view>

namespace WhiteoutDex {

enum class ImageUsage : i32 {
    Default   = 0,
    NormalMap = 1,
    ORM       = 2,
    Emissive  = 3,
    IBL       = 4,
};

ImageUsage DetermineImageUsage(std::string_view path);

inline bool IsLinearImageUsage(ImageUsage u) {
    return u == ImageUsage::NormalMap || u == ImageUsage::ORM;
}

gfx::Format ApplySrgbPolicy(gfx::Format raw, ImageUsage usage);

inline gfx::Format ApplyTextureSrgbPolicy(gfx::Format raw, std::string_view path) {
    return ApplySrgbPolicy(raw, DetermineImageUsage(path));
}

}
