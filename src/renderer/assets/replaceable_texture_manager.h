#pragma once

#include "../gfx/gfx.h"
#include "model/model_instance.h"
#include "whiteout/flakes/content_provider.h" // RequestId for Slot::pendingLoad
#include "whiteout/flakes/types.h"
#include "whiteout/flakes/util/replaceable_paths.h"

#include <atomic>
#include <unordered_map>
#include <vector>

namespace whiteout::flakes::io {
class IContentProvider;
}

namespace whiteout::flakes::renderer::assets {

class TextureAssetManager;

enum class ReplaceableKind : u8 {
    None = 0,
    TeamColor = 1,
    TeamGlow = 2,
};

class ReplaceableTextureManager {
public:
    ReplaceableTextureManager(gfx::IGFXDevice& gfx, TextureAssetManager& textures);
    ~ReplaceableTextureManager();

    ReplaceableTextureManager(const ReplaceableTextureManager&) = delete;
    ReplaceableTextureManager& operator=(const ReplaceableTextureManager&) = delete;

    void SetContentProvider(io::IContentProvider* p);

    // Re-bake every registered actor whose Actor::teamColorDirty bit is set,
    // clearing the bit. Hosts mutate team color via Actor::SetTeamColor (or
    // by writing teamColor + teamColorDirty directly); the renderer calls
    // this once per frame so the texture pixels catch up. Children inherit
    // their parent's color at spawn time; if the parent's color changes
    // after children exist, callers must mark children dirty explicitly
    // (this method does NOT walk parent→children relationships).
    void RebakeDirtyActors();

    void SetTileset(io::Tileset ts);

    bool ConsumeDirty() {
        return dirty_.exchange(false);
    }

    void RegisterModelSlot(model::Actor& mi, i32 textureId, i32 replaceableId);

    void UnregisterModel(model::Actor& mi);

    // Per-color swatch textures. The render paths pass the owning actor's
    // team color; this manager maintains a small color->texture cache so
    // multiple actors with the same color share one 1x1 texture.
    gfx::TextureHandle GetHdSwatchTextureFor(u32 rgba);
    gfx::TextureHandle GetSdTeamColorTextureFor(u32 rgba);
    gfx::TextureHandle GetSdTeamGlowTextureFor(u32 rgba);

    void Shutdown();

    struct DebugCounts {
        usize models = 0;
        usize slots = 0;
    };
    DebugCounts DebugSnapshot() const noexcept {
        DebugCounts c;
        c.models = slots_.size();
        for (auto& [mi, v] : slots_)
            c.slots += v.size();
        return c;
    }

private:
    gfx::IGFXDevice& gfx_;
    TextureAssetManager& textures_;

    std::atomic<bool> dirty_{false};

    // Color-keyed caches. One 1x1 texture per unique team color; small N.
    std::unordered_map<u32, gfx::TextureHandle> hdSwatchByColor_;
    std::unordered_map<u32, gfx::TextureHandle> sdTeamColorByColor_;
    std::unordered_map<u32, gfx::TextureHandle> sdTeamGlowByColor_;

    struct Slot {
        i32 textureId;
        u8 replaceableId;
        // Outstanding async load for the canonical asset (replaceable IDs
        // 11–37). kInvalidRequestId when no load is in flight. Cancelled
        // on re-bake (team color / tileset change) and on UnregisterModel
        // so the callback never writes into a destroyed actor's pixels.
        io::RequestId pendingLoad = io::kInvalidRequestId;
    };
    std::unordered_map<model::Actor*, std::vector<Slot>> slots_;

    io::IContentProvider* contentProvider_ = nullptr;

    void BakeSlot(model::Actor& mi, i32 textureId, i32 replaceableId);

    // Async completion for canonical-asset reads kicked off in BakeSlot.
    // Runs on the render thread via IContentProvider::Pump().
    void OnCanonicalAssetLoaded(model::Actor* mi, i32 textureId, i32 replaceableId,
                                io::RequestResult&& r);
};

} // namespace whiteout::flakes::renderer::assets
