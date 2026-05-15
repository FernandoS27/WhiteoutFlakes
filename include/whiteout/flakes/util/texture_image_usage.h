#pragma once

/// @file texture_image_usage.h
/// @brief Texture-usage policy helpers — decide sRGB-vs-linear sampling
///        intent from a texture's path/role and apply it to a Format.
///
/// Used by texture-loading adapters so the gfx layer picks the right
/// view (R8G8B8A8_UNORM_SRGB vs R8G8B8A8_UNORM) per-texture.

#include "../gfx_types.h"
#include "../types.h"

#include <string_view>

namespace whiteout::flakes::io {

/// @brief Semantic role of a texture in the material system.
///
/// Distinguishes sRGB colour data (`Default`, `Emissive`) from linear
/// data-tables (`NormalMap`, `ORM`, `IBL` cube faces) so the gfx layer
/// can pick the correct sampling colour space.
enum class ImageUsage : i32 {
    Default = 0,
    NormalMap = 1,
    ORM = 2,
    Emissive = 3,
    IBL = 4,
};

/// @brief Infer the `ImageUsage` from a texture's path-based naming hints
///        (e.g. `_nrm`, `_orm`, `_emit`, IBL probe suffixes).
ImageUsage DetermineImageUsage(std::string_view path);

/// @brief `true` if the usage demands linear sampling (no sRGB decode on
///        read), namely normal maps and ORM packed textures.
inline bool IsLinearImageUsage(ImageUsage u) {
    return u == ImageUsage::NormalMap || u == ImageUsage::ORM;
}

/// @brief Promote an UNORM format to its `_SRGB` variant (or vice-versa)
///        based on the texture's usage.
/// @param raw    Format as read from the file (typically UNORM).
/// @param usage  Semantic role; sRGB usages return the `_SRGB` variant
///               when one exists, linear usages return @p raw unchanged.
gfx::Format ApplySrgbPolicy(gfx::Format raw, ImageUsage usage);

/// @brief Convenience: `ApplySrgbPolicy(raw, DetermineImageUsage(path))`.
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
