#pragma once

#include "common_types.h"
#include "renderer/model/model_types.h"

#include <whiteout/textures/blp/blp.h>
#include <whiteout/textures/dds/parser.h>
#include <whiteout/textures/tga/parser.h>
#include <whiteout/textures/png/parser.h>
#include <whiteout/textures/texture.h>

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
    if      (fullBb) f |= BONE_BILLBOARD_FULL;
    else if (lockX)  f |= BONE_BILLBOARD_LOCK_X;
    else if (lockY)  f |= BONE_BILLBOARD_LOCK_Y;
    else if (lockZ)  f |= BONE_BILLBOARD_LOCK_Z;
    if (cameraAnchored) f |= BONE_BILLBOARD_CAMERA_ANCHORED;
    return f;
}

inline void FillSolidRGBA(std::vector<u8>& out, i32 w, i32 h,
                          u8 r, u8 g, u8 b, u8 a) {
    out.resize(usize(w) * usize(h) * 4);
    for (usize j = 0; j < out.size(); j += 4) {
        out[j] = r; out[j + 1] = g; out[j + 2] = b; out[j + 3] = a;
    }
}

inline std::string ExtensionLower(const std::filesystem::path& p) {
    std::string ext = p.extension().string();
    for (auto& c : ext) c = (char)std::tolower((unsigned char)c);
    return ext;
}

inline std::string NormalizeTextureKey(std::string_view path) {
    std::string out;
    out.reserve(path.size());
    bool started = false;
    for (char c : path) {
        if (!started) {
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') continue;
            started = true;
        }
        if (c == '\\') c = '/';
        else c = (char)std::tolower((unsigned char)c);
        out.push_back(c);
    }
    return out;
}

inline std::string NormalizeTextureKey(std::wstring_view path) {
    std::string narrow;
    narrow.reserve(path.size());
    for (wchar_t wc : path) narrow.push_back(static_cast<char>(wc));
    return NormalizeTextureKey(std::string_view(narrow));
}

template <typename ParseFn>
inline std::optional<whiteout::textures::Texture> DispatchTextureParser(
    const std::string& ext, ParseFn parse) {
    if (ext == ".blp") { whiteout::textures::blp::Parser p; return parse(p); }
    if (ext == ".dds") { whiteout::textures::dds::Parser p; return parse(p); }
    if (ext == ".tga") { whiteout::textures::tga::Parser p; return parse(p); }
    if (ext == ".png") { whiteout::textures::png::Parser p; return parse(p); }
    return std::nullopt;
}

inline bool DecodeToRGBA8(std::span<const u8> buf, const std::string& ext,
                          std::vector<u8>& out, i32& w, i32& h) {
    auto result = DispatchTextureParser(ext,
        [&](auto& parser) { return parser.parse(buf); });
    if (!result) return false;
    result->format(whiteout::textures::PixelFormat::RGBA8);
    w = (i32)result->width();
    h = (i32)result->height();
    auto mip0 = result->mipData(0);
    out.assign(mip0.begin(), mip0.end());
    return true;
}

}
