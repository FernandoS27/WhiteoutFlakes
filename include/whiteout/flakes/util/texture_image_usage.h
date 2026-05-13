#pragma once

// ============================================================================
// WhiteoutFlakes — texture-usage policy helpers.
//
// Determine sRGB-vs-linear sampling intent from a texture's path/role
// (NormalMap / ORM / Emissive / IBL / Default). Used by adapters that
// upload textures so the renderer's gfx layer picks the right pixel format.
// ============================================================================

#include "../gfx_types.h"
#include "../types.h"

#include <string_view>

namespace whiteout::flakes::io {

enum class ImageUsage : i32 {
    Default = 0,
    NormalMap = 1,
    ORM = 2,
    Emissive = 3,
    IBL = 4,
};

ImageUsage DetermineImageUsage(std::string_view path);

inline bool IsLinearImageUsage(ImageUsage u) {
    return u == ImageUsage::NormalMap || u == ImageUsage::ORM;
}

gfx::Format ApplySrgbPolicy(gfx::Format raw, ImageUsage usage);

inline gfx::Format ApplyTextureSrgbPolicy(gfx::Format raw, std::string_view path) {
    return ApplySrgbPolicy(raw, DetermineImageUsage(path));
}

} // namespace whiteout::flakes::io

namespace whiteout::flakes {
using ::whiteout::flakes::io::ApplySrgbPolicy;
using ::whiteout::flakes::io::ApplyTextureSrgbPolicy;
using ::whiteout::flakes::io::DetermineImageUsage;
using ::whiteout::flakes::io::ImageUsage;
using ::whiteout::flakes::io::IsLinearImageUsage;
} // namespace whiteout::flakes
