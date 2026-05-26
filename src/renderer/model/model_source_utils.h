#pragma once

#include "whiteout/flakes/model_types.h"
#include "whiteout/flakes/types.h"

#include <whiteout/textures/blp/blp.h>
#include <whiteout/textures/dds/parser.h>
#include <whiteout/textures/png/parser.h>
#include <whiteout/textures/texture.h>
#include <whiteout/textures/tga/parser.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <type_traits>
#include <vector>

namespace whiteout::flakes::renderer::model {

template <typename Enum, std::enable_if_t<std::is_enum_v<Enum>, i32> = 0>
inline bool hasFlag(u32 bits, Enum flag) {
    return (bits & static_cast<u32>(flag)) != 0;
}

inline u32 PackBillboardFlags(bool fullBb, bool lockX, bool lockY, bool lockZ,
                              bool cameraAnchored) {
    u32 f = 0;
    if (fullBb)
        f |= BONE_BILLBOARD_FULL;
    else if (lockX)
        f |= BONE_BILLBOARD_LOCK_X;
    else if (lockY)
        f |= BONE_BILLBOARD_LOCK_Y;
    else if (lockZ)
        f |= BONE_BILLBOARD_LOCK_Z;
    if (cameraAnchored)
        f |= BONE_BILLBOARD_CAMERA_ANCHORED;
    return f;
}

inline void FillSolidRGBA(std::vector<u8>& out, i32 w, i32 h, u8 r, u8 g, u8 b, u8 a) {
    out.resize(usize(w) * usize(h) * 4);
    for (usize j = 0; j < out.size(); j += 4) {
        out[j] = r;
        out[j + 1] = g;
        out[j + 2] = b;
        out[j + 3] = a;
    }
}

inline std::string ExtensionLower(const std::filesystem::path& p) {
    std::string ext = p.extension().string();
    for (auto& c : ext)
        c = (char)std::tolower((unsigned char)c);
    return ext;
}

inline std::string NormalizeTextureKey(std::string_view path) {
    std::string out;
    out.reserve(path.size());
    bool started = false;
    for (char c : path) {
        if (!started) {
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
                continue;
            started = true;
        }
        if (c == '\\')
            c = '/';
        else
            c = (char)std::tolower((unsigned char)c);
        out.push_back(c);
    }
    return out;
}

inline std::string NormalizeTextureKey(std::wstring_view path) {
    std::string narrow;
    narrow.reserve(path.size());
    for (wchar_t wc : path)
        narrow.push_back(static_cast<char>(wc));
    return NormalizeTextureKey(std::string_view(narrow));
}

template <typename ParseFn>
inline std::optional<whiteout::textures::Texture> DispatchTextureParser(const std::string& ext,
                                                                        ParseFn parse) {
    if (ext == ".blp") {
        whiteout::textures::blp::Parser p;
        return parse(p);
    }
    if (ext == ".dds") {
        whiteout::textures::dds::Parser p;
        return parse(p);
    }
    if (ext == ".tga") {
        whiteout::textures::tga::Parser p;
        return parse(p);
    }
    if (ext == ".png") {
        whiteout::textures::png::Parser p;
        return parse(p);
    }
    return std::nullopt;
}

inline bool DecodeToRGBA8(std::span<const u8> buf, const std::string& ext, std::vector<u8>& out,
                          i32& w, i32& h) {
    auto result = DispatchTextureParser(ext, [&](auto& parser) { return parser.parse(buf); });
    if (!result)
        return false;
    result->format(whiteout::textures::PixelFormat::RGBA8);
    w = (i32)result->width();
    h = (i32)result->height();
    auto mip0 = result->mipData(0);
    out.assign(mip0.begin(), mip0.end());
    return true;
}

// WhiteoutLib PixelFormat → gfx::Format. The MDX texture parser
// preserves the source's native format (BLP1/BC1/BC3 etc.) and the
// renderer's BLS pipeline can sample any of them directly — we only
// fall through to RGBA8 when the source is in a format the gfx
// backend doesn't speak. Used by AssetManager's texture-apply path
// to keep BC compression + the full mip chain when possible.
inline gfx::Format WhiteoutFormatToGfx(whiteout::textures::PixelFormat pf, bool srgb) {
    using PF = whiteout::textures::PixelFormat;
    switch (pf) {
    case PF::R8:      return gfx::Format::R8_UNORM;
    case PF::R16:     return gfx::Format::R16_UNORM;
    case PF::R32F:    return gfx::Format::R32_FLOAT;
    case PF::RG8:     return gfx::Format::R8G8_UNORM;
    case PF::RG16:    return gfx::Format::R16G16_UNORM;
    case PF::RG32F:   return gfx::Format::R32G32_FLOAT;
    case PF::RGBA8:   return srgb ? gfx::Format::R8G8B8A8_UNORM_SRGB : gfx::Format::R8G8B8A8_UNORM;
    case PF::RGBA16:  return gfx::Format::R16G16B16A16_UNORM;
    case PF::RGBA32F: return gfx::Format::R32G32B32A32_FLOAT;
    case PF::BC1:     return srgb ? gfx::Format::BC1_UNORM_SRGB : gfx::Format::BC1_UNORM;
    case PF::BC2:     return srgb ? gfx::Format::BC2_UNORM_SRGB : gfx::Format::BC2_UNORM;
    case PF::BC3:     return srgb ? gfx::Format::BC3_UNORM_SRGB : gfx::Format::BC3_UNORM;
    case PF::BC4:     return gfx::Format::BC4_UNORM;
    case PF::BC5:     return gfx::Format::BC5_UNORM;
    case PF::BC6H:    return gfx::Format::BC6H_UF16;
    case PF::BC7:     return srgb ? gfx::Format::BC7_UNORM_SRGB : gfx::Format::BC7_UNORM;
    }
    return gfx::Format::Unknown;
}

} // namespace whiteout::flakes::renderer::model
