#include "renderer/replaceable_texture_manager.h"
#include "renderer/texture_asset_manager.h"
#include "team_glow_data.h"
#include "io/content_provider.h"
#include "io/event_data.h"
#include "io/replaceable_paths.h"
#include "model_source_utils.h"

#include <cstdio>
#include <filesystem>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace WhiteoutDex {

namespace {

inline u8 Red  (u32 bgr) noexcept { return (u8)(bgr        & 0xFF); }
inline u8 Green(u32 bgr) noexcept { return (u8)((bgr >> 8) & 0xFF); }
inline u8 Blue (u32 bgr) noexcept { return (u8)((bgr >>16) & 0xFF); }
}

ReplaceableTextureManager::ReplaceableTextureManager(gfx::IGFXDevice& gfx,
                                                     TextureAssetManager& textures)
    : gfx_(gfx), textures_(textures) {}

void ReplaceableTextureManager::SetContentProvider(IContentProvider* p) {
    contentProvider_ = p;

    io::LoadGameDataFiles(p);

    io::LoadEventDataFiles(p);
}

ReplaceableTextureManager::~ReplaceableTextureManager() {
    Shutdown();
}

void ReplaceableTextureManager::Shutdown() {
    if (hdSwatchTex_ != gfx::TextureHandle::Invalid) {
        gfx_.Destroy(hdSwatchTex_);
        hdSwatchTex_    = gfx::TextureHandle::Invalid;
        lastSwatchRgba_ = 0xFFFFFFFFu;
    }
    if (sdTeamColorTex_ != gfx::TextureHandle::Invalid) {
        gfx_.Destroy(sdTeamColorTex_);
        sdTeamColorTex_ = gfx::TextureHandle::Invalid;
    }
    if (sdTeamGlowTex_ != gfx::TextureHandle::Invalid) {
        gfx_.Destroy(sdTeamGlowTex_);
        sdTeamGlowTex_ = gfx::TextureHandle::Invalid;
    }
    lastSdSwatchRgba_ = 0xFFFFFFFFu;
    slots_.clear();
}

void ReplaceableTextureManager::SetTeamColor(u8 r, u8 g, u8 b) {

    teamColor_ = (u32)r | ((u32)g << 8) | ((u32)b << 16);

    for (auto& [mi, slots] : slots_) {
        for (auto& s : slots) BakeSlot(*mi, s.textureId, (i32)s.replaceableId);
    }

    if (sdTeamColorTex_ != gfx::TextureHandle::Invalid) {
        gfx_.Destroy(sdTeamColorTex_);
        sdTeamColorTex_ = gfx::TextureHandle::Invalid;
    }
    if (sdTeamGlowTex_ != gfx::TextureHandle::Invalid) {
        gfx_.Destroy(sdTeamGlowTex_);
        sdTeamGlowTex_ = gfx::TextureHandle::Invalid;
    }
    lastSdSwatchRgba_ = 0xFFFFFFFFu;
    dirty_.store(true);
}

void ReplaceableTextureManager::SetTileset(io::Tileset ts) {
    io::SetCurrentTileset(ts);

    for (auto& [mi, slots] : slots_) {
        for (auto& s : slots) {
            if (s.replaceableId >= 11 && s.replaceableId <= 14)
                BakeSlot(*mi, s.textureId, (i32)s.replaceableId);
        }
    }
    dirty_.store(true);
}

void ReplaceableTextureManager::RegisterModelSlot(Actor& mi,
                                                  i32    textureId,
                                                  i32    replaceableId) {

    const bool isTeamColor = (replaceableId == 1);
    const bool isTeamGlow  = (replaceableId == 2);
    const bool isCanonical = (replaceableId >= 11 && replaceableId <= 37);
    if (!isTeamColor && !isTeamGlow && !isCanonical) return;

    auto& list = slots_[&mi];

    for (auto& s : list)
        if (s.textureId == textureId && (i32)s.replaceableId == replaceableId) return;
    for (auto& s : list)
        if (s.textureId == textureId && (i32)s.replaceableId != replaceableId) {
            char msg[160];
            std::snprintf(msg, sizeof(msg),
                "[WDEX replaceable] textureId %d registered with replaceableId %d AND %d "
                "— second registration overwrites the first's pixels on bake.\n",
                textureId, (i32)s.replaceableId, replaceableId);
            OutputDebugStringA(msg);
            break;
        }
    list.push_back({textureId, static_cast<u8>(replaceableId)});
    BakeSlot(mi, textureId, replaceableId);
}

void ReplaceableTextureManager::UnregisterModel(Actor& mi) {
    slots_.erase(&mi);
}

namespace {

bool DecodeCanonicalAsset(IContentProvider& cp, const std::string& path,
                          std::vector<u8>& outPixels, i32& outW, i32& outH) {
    std::string foundExt;
    auto data = cp.ReadFile(path, &foundExt);
    if (!data) {
        std::fprintf(stderr,
                     "[textures] ERR: ReplaceableTexture read FAIL '%s'\n",
                     path.c_str());
        return false;
    }
    if (foundExt.empty()) foundExt = ExtensionLower(std::filesystem::path(path));

    auto result = DispatchTextureParser(foundExt,
        [&](auto& parser) { return parser.parse(*data); });
    if (!result) {
        std::fprintf(stderr,
                     "[textures] ERR: ReplaceableTexture decode FAIL '%s' "
                     "ext='%s' bytes=%zu\n",
                     path.c_str(), foundExt.c_str(), data->size());
        return false;
    }

    result->format(whiteout::textures::PixelFormat::RGBA8);
    outW = (i32)result->width();
    outH = (i32)result->height();
    if (outW <= 0 || outH <= 0) {
        std::fprintf(stderr,
                     "[textures] ERR: ReplaceableTexture invalid size '%s' "
                     "%dx%d\n",
                     path.c_str(), outW, outH);
        return false;
    }
    auto mip0 = result->mipData(0);
    outPixels.assign(mip0.begin(), mip0.end());
    return true;
}
}

void ReplaceableTextureManager::BakeSlot(Actor& mi,
                                         i32    textureId,
                                         i32    replaceableId) {
    const u8 r = Red(teamColor_);
    const u8 g = Green(teamColor_);
    const u8 b = Blue(teamColor_);

    StagedTexture& st = mi.render.stagedTextures[textureId];
    st.replaceableId = replaceableId;

    st.format    = gfx::Format::R8G8B8A8_UNORM;
    st.mipLevels = 1;

    if (replaceableId == 2) {

        st.pixels = DecodeTeamGlow(r, g, b, st.width, st.height);
    } else if (replaceableId == 1) {

        st.width  = 4;
        st.height = 4;
        st.pixels.resize(64);
        for (i32 j = 0; j < 16; j++) {
            st.pixels[j*4 + 0] = r;
            st.pixels[j*4 + 1] = g;
            st.pixels[j*4 + 2] = b;
            st.pixels[j*4 + 3] = 255;
        }
    } else {

        const char* canon = io::ReplaceableCanonicalPath(replaceableId);
        bool loaded = false;
        if (canon && contentProvider_) {
            loaded = DecodeCanonicalAsset(*contentProvider_, canon,
                                          st.pixels, st.width, st.height);
        }
        if (!loaded) {
            st.width  = 4;
            st.height = 4;
            st.pixels.assign(64, 0);
            for (i32 j = 0; j < 16; ++j) {
                st.pixels[j*4 + 0] = 255;
                st.pixels[j*4 + 1] = 0;
                st.pixels[j*4 + 2] = 255;
                st.pixels[j*4 + 3] = 255;
            }
        }
    }
    mi.render.stagedDirty = true;
}

gfx::TextureHandle ReplaceableTextureManager::GetHdSwatchTexture() {

    const u8 r = Red(teamColor_);
    const u8 g = Green(teamColor_);
    const u8 b = Blue(teamColor_);
    const u32 rgba =
        (u32)r | ((u32)g << 8) | ((u32)b << 16) | 0xFF000000u;

    if (hdSwatchTex_ != gfx::TextureHandle::Invalid && lastSwatchRgba_ == rgba)
        return hdSwatchTex_;
    if (hdSwatchTex_ != gfx::TextureHandle::Invalid) {
        gfx_.Destroy(hdSwatchTex_);
        hdSwatchTex_ = gfx::TextureHandle::Invalid;
    }
    u32 px = rgba;
    hdSwatchTex_ = gfx_.CreateTexture({
        .width  = 1,
        .height = 1,
        .format = gfx::Format::R8G8B8A8_UNORM,
        .usage  = gfx::TextureUsage::ShaderResource,
    }, &px);
    lastSwatchRgba_ = rgba;
    return hdSwatchTex_;
}

gfx::TextureHandle ReplaceableTextureManager::GetSdTeamColorTexture() {

    const u8 r = Red(teamColor_);
    const u8 g = Green(teamColor_);
    const u8 b = Blue(teamColor_);
    const u32 rgba =
        (u32)r | ((u32)g << 8) | ((u32)b << 16) | 0xFF000000u;
    if (sdTeamColorTex_ != gfx::TextureHandle::Invalid && lastSdSwatchRgba_ == rgba)
        return sdTeamColorTex_;
    if (sdTeamColorTex_ != gfx::TextureHandle::Invalid) {
        gfx_.Destroy(sdTeamColorTex_);
        sdTeamColorTex_ = gfx::TextureHandle::Invalid;
    }
    u32 px[16];
    for (i32 i = 0; i < 16; ++i) px[i] = rgba;
    sdTeamColorTex_ = gfx_.CreateTexture({
        .width  = 4,
        .height = 4,
        .format = gfx::Format::R8G8B8A8_UNORM,
        .usage  = gfx::TextureUsage::ShaderResource,
    }, px);
    lastSdSwatchRgba_ = rgba;
    return sdTeamColorTex_;
}

gfx::TextureHandle ReplaceableTextureManager::GetSdTeamGlowTexture() {

    const u8 r = Red(teamColor_);
    const u8 g = Green(teamColor_);
    const u8 b = Blue(teamColor_);
    const u32 rgba =
        (u32)r | ((u32)g << 8) | ((u32)b << 16) | 0xFF000000u;
    if (sdTeamGlowTex_ != gfx::TextureHandle::Invalid && lastSdSwatchRgba_ == rgba)
        return sdTeamGlowTex_;
    if (sdTeamGlowTex_ != gfx::TextureHandle::Invalid) {
        gfx_.Destroy(sdTeamGlowTex_);
        sdTeamGlowTex_ = gfx::TextureHandle::Invalid;
    }
    i32 w = 0, h = 0;
    std::vector<u8> pixels = DecodeTeamGlow(r, g, b, w, h);
    if (w <= 0 || h <= 0 || pixels.empty()) return gfx::TextureHandle::Invalid;
    sdTeamGlowTex_ = gfx_.CreateTexture({
        .width  = w,
        .height = h,
        .format = gfx::Format::R8G8B8A8_UNORM,
        .usage  = gfx::TextureUsage::ShaderResource,
    }, pixels.data());
    lastSdSwatchRgba_ = rgba;
    return sdTeamGlowTex_;
}

}
