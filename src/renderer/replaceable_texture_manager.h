#pragma once

#include "../gfx/gfx.h"
#include "../io/replaceable_paths.h"
#include "common_types.h"
#include "model_instance.h"

#include <atomic>
#include <unordered_map>
#include <vector>

namespace WhiteoutDex {

class TextureAssetManager;
class IContentProvider;

enum class ReplaceableKind : u8 {
    None      = 0,
    TeamColor = 1,
    TeamGlow  = 2,
};

class ReplaceableTextureManager {
public:
    ReplaceableTextureManager(gfx::IGFXDevice& gfx, TextureAssetManager& textures);
    ~ReplaceableTextureManager();

    ReplaceableTextureManager(const ReplaceableTextureManager&)            = delete;
    ReplaceableTextureManager& operator=(const ReplaceableTextureManager&) = delete;

    void SetContentProvider(IContentProvider* p);

    void SetTeamColor(u8 r, u8 g, u8 b);

    u32 GetTeamColorRaw() const noexcept { return teamColor_; }

    void SetTileset(io::Tileset ts);

    bool ConsumeDirty() { return dirty_.exchange(false); }

    void RegisterModelSlot(Actor& mi, i32 textureId, i32 replaceableId);

    void UnregisterModel(Actor& mi);

    gfx::TextureHandle GetHdSwatchTexture();

    gfx::TextureHandle GetSdTeamColorTexture();
    gfx::TextureHandle GetSdTeamGlowTexture();

    void Shutdown();

    struct DebugCounts { usize models = 0; usize slots = 0; };
    DebugCounts DebugSnapshot() const noexcept {
        DebugCounts c;
        c.models = slots_.size();
        for (auto& [mi, v] : slots_) c.slots += v.size();
        return c;
    }

private:
    gfx::IGFXDevice&     gfx_;
    TextureAssetManager& textures_;

    u32               teamColor_ = 0x000000FFu;
    std::atomic<bool> dirty_{false};

    gfx::TextureHandle hdSwatchTex_    = gfx::TextureHandle::Invalid;
    u32                lastSwatchRgba_ = 0xFFFFFFFFu;

    gfx::TextureHandle sdTeamColorTex_     = gfx::TextureHandle::Invalid;
    gfx::TextureHandle sdTeamGlowTex_      = gfx::TextureHandle::Invalid;
    u32                lastSdSwatchRgba_   = 0xFFFFFFFFu;

    struct Slot { i32 textureId; u8 replaceableId; };
    std::unordered_map<Actor*, std::vector<Slot>> slots_;

    IContentProvider* contentProvider_ = nullptr;

    void BakeSlot(Actor& mi, i32 textureId, i32 replaceableId);
};

}
