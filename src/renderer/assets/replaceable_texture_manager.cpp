#include "dbg_print.h"
#include "model/model_source_utils.h"
#include "renderer/assets/replaceable_texture_manager.h"
#include "renderer/assets/texture_asset_manager.h"
#include "renderer/assets/texture_stub.h"
#include "whiteout/flakes/content_provider.h"
#include "whiteout/flakes/event_data.h"
#include "whiteout/flakes/util/replaceable_paths.h"
#include "whiteout/flakes/util/team_glow_data.h"

#include <cstdio>
#include <filesystem>
#include <utility>

namespace whiteout::flakes::renderer::assets {

using namespace ::whiteout::flakes::renderer::model;
using namespace ::whiteout::flakes::io;

namespace {

inline u8 Red(u32 bgr) noexcept {
    return (u8)(bgr & 0xFF);
}
inline u8 Green(u32 bgr) noexcept {
    return (u8)((bgr >> 8) & 0xFF);
}
inline u8 Blue(u32 bgr) noexcept {
    return (u8)((bgr >> 16) & 0xFF);
}
} // namespace

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
    auto destroyAll = [&](std::unordered_map<u32, gfx::TextureHandle>& m) {
        for (auto& [k, h] : m)
            if (h != gfx::TextureHandle::Invalid)
                gfx_.Destroy(h);
        m.clear();
    };
    destroyAll(hdSwatchByColor_);
    destroyAll(sdTeamColorByColor_);
    destroyAll(sdTeamGlowByColor_);
    slots_.clear();
}

void ReplaceableTextureManager::RebakeDirtyActors() {
    bool anyRebaked = false;
    for (auto& [actorPtr, slots] : slots_) {
        if (!actorPtr->teamColorDirty)
            continue;
        for (auto& s : slots)
            BakeSlot(*actorPtr, s.textureId, (i32)s.replaceableId);
        actorPtr->teamColorDirty = false;
        anyRebaked = true;
    }
    if (anyRebaked)
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

void ReplaceableTextureManager::RegisterModelSlot(Actor& mi, i32 textureId, i32 replaceableId) {

    const bool isTeamColor = (replaceableId == 1);
    const bool isTeamGlow = (replaceableId == 2);
    const bool isCanonical = (replaceableId >= 11 && replaceableId <= 37);
    if (!isTeamColor && !isTeamGlow && !isCanonical)
        return;

    auto& list = slots_[&mi];

    for (auto& s : list)
        if (s.textureId == textureId && (i32)s.replaceableId == replaceableId)
            return;
    for (auto& s : list)
        if (s.textureId == textureId && (i32)s.replaceableId != replaceableId) {
            char msg[160];
            std::snprintf(msg, sizeof(msg),
                          "[WDEX replaceable] textureId %d registered with replaceableId %d AND %d "
                          "— second registration overwrites the first's pixels on bake.\n",
                          textureId, (i32)s.replaceableId, replaceableId);
            DbgPrint(msg);
            break;
        }
    list.push_back({textureId, static_cast<u8>(replaceableId)});
    BakeSlot(mi, textureId, replaceableId);
}

void ReplaceableTextureManager::UnregisterModel(Actor& mi) {
    auto it = slots_.find(&mi);
    if (it == slots_.end())
        return;
    // Cancel any in-flight canonical-asset loads so the completion callback
    // doesn't write through a dangling Actor* once the slots are gone.
    // FileContentProvider::Pump() suppresses already-queued completions for
    // cancelled IDs, so even a callback that's mid-flight in the worker is
    // safe.
    if (contentProvider_) {
        for (auto& s : it->second) {
            if (s.pendingLoad != io::kInvalidRequestId)
                contentProvider_->Cancel(s.pendingLoad);
        }
    }
    slots_.erase(it);
}

void ReplaceableTextureManager::BakeSlot(Actor& mi, i32 textureId, i32 replaceableId) {
    const u8 r = Red(mi.teamColor);
    const u8 g = Green(mi.teamColor);
    const u8 b = Blue(mi.teamColor);

    StagedTexture& st = mi.render.stagedTextures[textureId];
    st.replaceableId = replaceableId;

    st.format = gfx::Format::R8G8B8A8_UNORM;
    st.mipLevels = 1;

    if (replaceableId == 2) {
        // Team glow is a synthesized texture (no archive read needed).
        st.pixels = DecodeTeamGlow(r, g, b, st.width, st.height);
        mi.render.stagedDirty = true;
        return;
    }
    if (replaceableId == 1) {
        // Team color swatch — same idea, no IO.
        st.width = 4;
        st.height = 4;
        st.pixels.resize(64);
        for (i32 j = 0; j < 16; j++) {
            st.pixels[j * 4 + 0] = r;
            st.pixels[j * 4 + 1] = g;
            st.pixels[j * 4 + 2] = b;
            st.pixels[j * 4 + 3] = 255;
        }
        mi.render.stagedDirty = true;
        return;
    }

    // ---- Canonical asset (replaceable IDs 11–37): stub now, real bytes
    // ---- later via the async content provider.
    //
    // The diffuse stub keeps the model visible at neutral-white the first
    // frame the actor is drawn; the worker thread reads the archive in the
    // background and OnCanonicalAssetLoaded swaps real pixels in. Re-baking
    // the same slot (team color / tileset change) cancels the prior load
    // first so a slow earlier read can never overwrite a faster later one.
    {
        const auto stubPx = StubPixelRGBA(TextureChannelKind::Diffuse);
        st.width = 1;
        st.height = 1;
        st.pixels.assign(stubPx.begin(), stubPx.end());
        mi.render.stagedDirty = true;
    }

    const char* canon = io::ReplaceableCanonicalPath(replaceableId);
    if (!canon || !contentProvider_)
        return;

    // Locate the matching Slot to attach the new request id. RegisterModelSlot
    // pushes the entry before calling BakeSlot, so it must exist.
    auto it = slots_.find(&mi);
    if (it == slots_.end())
        return;
    Slot* slot = nullptr;
    for (auto& s : it->second) {
        if (s.textureId == textureId && (i32)s.replaceableId == replaceableId) {
            slot = &s;
            break;
        }
    }
    if (!slot)
        return;
    if (slot->pendingLoad != io::kInvalidRequestId)
        contentProvider_->Cancel(slot->pendingLoad);

    Actor* miPtr = &mi;
    slot->pendingLoad = contentProvider_->Request(
        canon, [this, miPtr, textureId, replaceableId](io::RequestResult&& r) {
            OnCanonicalAssetLoaded(miPtr, textureId, replaceableId, std::move(r));
        });
}

void ReplaceableTextureManager::OnCanonicalAssetLoaded(Actor* miPtr, i32 textureId,
                                                       i32 replaceableId, io::RequestResult&& r) {
    // The actor may have been unregistered (and possibly destroyed) between
    // request submission and this callback. UnregisterModel cancels pending
    // loads, but a late-completion path through Pump still suppresses cancelled
    // ids — defensively, also re-check the slots_ map so we never write through
    // a stale Actor*.
    auto it = slots_.find(miPtr);
    if (it == slots_.end())
        return;
    Slot* slot = nullptr;
    for (auto& s : it->second) {
        if (s.textureId == textureId && (i32)s.replaceableId == replaceableId) {
            slot = &s;
            break;
        }
    }
    if (!slot)
        return;
    slot->pendingLoad = io::kInvalidRequestId;

    if (!r.ok || r.data.empty()) {
        // Keep the white stub; the real asset just didn't exist or read failed.
        std::fprintf(stderr, "[textures] ReplaceableTexture async read miss for id %d\n",
                     replaceableId);
        return;
    }

    std::string foundExt = r.actualExt;
    if (foundExt.empty()) {
        if (const char* canon = io::ReplaceableCanonicalPath(replaceableId))
            foundExt = ExtensionLower(std::filesystem::path(canon));
    }

    auto result =
        DispatchTextureParser(foundExt, [&](auto& parser) { return parser.parse(r.data); });
    if (!result) {
        std::fprintf(stderr,
                     "[textures] ReplaceableTexture decode FAIL ext='%s' bytes=%zu (id %d)\n",
                     foundExt.c_str(), r.data.size(), replaceableId);
        return;
    }
    result->format(whiteout::textures::PixelFormat::RGBA8);
    const i32 w = (i32)result->width();
    const i32 h = (i32)result->height();
    if (w <= 0 || h <= 0)
        return;
    auto mip0 = result->mipData(0);

    StagedTexture& st = miPtr->render.stagedTextures[textureId];
    st.width = w;
    st.height = h;
    st.pixels.assign(mip0.begin(), mip0.end());
    miPtr->render.stagedDirty = true;
    dirty_.store(true);
}

gfx::TextureHandle ReplaceableTextureManager::GetHdSwatchTextureFor(u32 rgba) {
    auto it = hdSwatchByColor_.find(rgba);
    if (it != hdSwatchByColor_.end())
        return it->second;

    u32 px = rgba;
    auto h = gfx_.CreateTexture(
        {
            .width = 1,
            .height = 1,
            .format = gfx::Format::R8G8B8A8_UNORM,
            .usage = gfx::TextureUsage::ShaderResource,
        },
        &px);
    hdSwatchByColor_.emplace(rgba, h);
    return h;
}

gfx::TextureHandle ReplaceableTextureManager::GetSdTeamColorTextureFor(u32 rgba) {
    auto it = sdTeamColorByColor_.find(rgba);
    if (it != sdTeamColorByColor_.end())
        return it->second;

    u32 px[16];
    for (i32 i = 0; i < 16; ++i)
        px[i] = rgba;
    auto h = gfx_.CreateTexture(
        {
            .width = 4,
            .height = 4,
            .format = gfx::Format::R8G8B8A8_UNORM,
            .usage = gfx::TextureUsage::ShaderResource,
        },
        px);
    sdTeamColorByColor_.emplace(rgba, h);
    return h;
}

gfx::TextureHandle ReplaceableTextureManager::GetSdTeamGlowTextureFor(u32 rgba) {
    auto it = sdTeamGlowByColor_.find(rgba);
    if (it != sdTeamGlowByColor_.end())
        return it->second;

    const u8 r = (u8)(rgba & 0xFF);
    const u8 g = (u8)((rgba >> 8) & 0xFF);
    const u8 b = (u8)((rgba >> 16) & 0xFF);
    i32 w = 0, hgt = 0;
    std::vector<u8> pixels = DecodeTeamGlow(r, g, b, w, hgt);
    if (w <= 0 || hgt <= 0 || pixels.empty())
        return gfx::TextureHandle::Invalid;
    auto h = gfx_.CreateTexture(
        {
            .width = w,
            .height = hgt,
            .format = gfx::Format::R8G8B8A8_UNORM,
            .usage = gfx::TextureUsage::ShaderResource,
        },
        pixels.data());
    sdTeamGlowByColor_.emplace(rgba, h);
    return h;
}

} // namespace whiteout::flakes::renderer::assets
