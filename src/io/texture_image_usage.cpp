#include "whiteout/flakes/types.h"
#include "whiteout/flakes/util/texture_image_usage.h"

#include <algorithm>
#include <cctype>
#include <string>

namespace whiteout::flakes::io {

namespace {

std::string NormaliseStem(std::string_view path) {
    std::string out;
    out.reserve(path.size());

    const usize lastSep = path.find_last_of("/\\");
    const usize scanFrom = (lastSep == std::string_view::npos) ? 0u : lastSep + 1u;
    const usize lastDot = path.find_last_of('.');
    const usize end =
        (lastDot != std::string_view::npos && lastDot >= scanFrom) ? lastDot : path.size();
    for (usize i = 0; i < end; ++i) {
        char c = path[i];
        if (c == '\\')
            c = '/';
        out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

bool EndsWith(std::string_view s, std::string_view suffix) {
    return s.size() >= suffix.size() &&
           std::equal(suffix.begin(), suffix.end(), s.end() - suffix.size());
}

bool Contains(std::string_view haystack, std::string_view needle) {
    return haystack.find(needle) != std::string_view::npos;
}

} // namespace

ImageUsage DetermineImageUsage(std::string_view path) {
    const std::string stem = NormaliseStem(path);

    if (EndsWith(stem, "_diffuse"))
        return ImageUsage::Default;
    if (EndsWith(stem, "_normal"))
        return ImageUsage::NormalMap;
    if (EndsWith(stem, "_orm"))
        return ImageUsage::ORM;
    if (EndsWith(stem, "_emissive"))
        return ImageUsage::Emissive;
    if (EndsWith(stem, "_ibl"))
        return ImageUsage::IBL;

    if (Contains(stem, "textures/normal"))
        return ImageUsage::NormalMap;
    if (Contains(stem, "textures/orm"))
        return ImageUsage::ORM;

    return ImageUsage::Default;
}

gfx::Format ApplySrgbPolicy(gfx::Format raw, ImageUsage usage) {
    const bool wantLinear = IsLinearImageUsage(usage);

    auto stripSrgb = [](gfx::Format f) {
        switch (f) {
        case gfx::Format::R8G8B8A8_UNORM_SRGB:
            return gfx::Format::R8G8B8A8_UNORM;
        case gfx::Format::BC1_UNORM_SRGB:
            return gfx::Format::BC1_UNORM;
        case gfx::Format::BC2_UNORM_SRGB:
            return gfx::Format::BC2_UNORM;
        case gfx::Format::BC3_UNORM_SRGB:
            return gfx::Format::BC3_UNORM;
        case gfx::Format::BC7_UNORM_SRGB:
            return gfx::Format::BC7_UNORM;
        default:
            return f;
        }
    };
    auto promoteSrgb = [](gfx::Format f) {
        switch (f) {
        case gfx::Format::R8G8B8A8_UNORM:
            return gfx::Format::R8G8B8A8_UNORM_SRGB;
        case gfx::Format::BC1_UNORM:
            return gfx::Format::BC1_UNORM_SRGB;
        case gfx::Format::BC2_UNORM:
            return gfx::Format::BC2_UNORM_SRGB;
        case gfx::Format::BC3_UNORM:
            return gfx::Format::BC3_UNORM_SRGB;
        case gfx::Format::BC7_UNORM:
            return gfx::Format::BC7_UNORM_SRGB;
        default:
            return f;
        }
    };
    return wantLinear ? stripSrgb(raw) : promoteSrgb(raw);
}

} // namespace whiteout::flakes::io
