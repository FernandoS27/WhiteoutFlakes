#pragma once

#include "../gfx/gfx.h"
#include "assets/sampler_asset_manager.h"
#include "whiteout/flakes/types.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace whiteout::flakes::renderer::assets {

class AssetManager;

class TextureAssetManager {
public:
    explicit TextureAssetManager(gfx::IGFXDevice& gfx);
    ~TextureAssetManager();

    TextureAssetManager(const TextureAssetManager&) = delete;
    TextureAssetManager& operator=(const TextureAssetManager&) = delete;

    class ModelScope {
    public:
        /// @brief Upload a one-shot, scope-owned texture (no AssetManager
        ///        slot). Used for the rare MDX texture entries that have
        ///        no file path — e.g. the 4x4 white synthetic that the
        ///        adapter generates for fully-procedural texture slots.
        gfx::TextureHandle Upload(i32 textureId, const gfx::TextureDesc& desc, const void* pixels,
                                  u32 wrapFlags);

        /// @brief Bind this texture id to an AssetManager slot. The
        ///        slot's refcount is incremented; ModelScope releases it
        ///        on DropEntry / Clear. `Get()` resolves through the
        ///        manager so the actor sees the current handle whenever
        ///        the slot's payload is swapped (placeholder → real).
        void BindSlot(i32 textureId, std::uint32_t slot, u32 wrapFlags);

        gfx::TextureHandle Get(i32 textureId) const noexcept;

        u32 WrapFlags(i32 textureId) const noexcept;

        usize Size() const noexcept {
            return entries_.size();
        }

        void Clear();

        ~ModelScope();
        ModelScope(const ModelScope&) = delete;
        ModelScope& operator=(const ModelScope&) = delete;

    private:
        friend class TextureAssetManager;
        ModelScope(gfx::IGFXDevice& gfx, TextureAssetManager& mgr, AssetManager* assets)
            : gfx_(gfx), mgr_(mgr), assets_(assets) {}

        void DropEntry(i32 textureId);

        gfx::IGFXDevice& gfx_;
        TextureAssetManager& mgr_;
        AssetManager* assets_ = nullptr; // null = legacy scope, no slot routing
        struct Entry {
            // Either holds an owned GPU handle (Upload path) or a slot
            // ref into AssetManager. Exactly one of `tex` / `slot` is
            // populated; Get() picks the right one.
            gfx::TextureHandle tex = gfx::TextureHandle::Invalid;
            std::uint32_t slot     = 0; // AssetManager::kInvalidSlot
            u32 wrapFlags          = kSamplerWrapBitsMask;
        };
        std::unordered_map<i32, Entry> entries_;
    };

    std::unique_ptr<ModelScope> CreateModelScope(AssetManager* assets = nullptr);

    void RegisterOwned(std::string name, gfx::TextureHandle handle);
    void ReleaseOwned(std::string_view name);
    gfx::TextureHandle GetOwned(std::string_view name) const noexcept;

    struct DebugEntry {
        std::string name;
        gfx::TextureHandle handle;
    };
    std::vector<DebugEntry> DebugSnapshotOwned() const;

    struct Defaults {
        gfx::TextureHandle White = gfx::TextureHandle::Invalid;
        gfx::TextureHandle Black = gfx::TextureHandle::Invalid;
        gfx::TextureHandle FlatNormal = gfx::TextureHandle::Invalid;
        gfx::TextureHandle NeutralOrm = gfx::TextureHandle::Invalid;
        gfx::TextureHandle Missing = gfx::TextureHandle::Invalid;
    };
    const Defaults& GetDefaults() const noexcept {
        return defaults_;
    }

private:
    gfx::IGFXDevice& gfx_;
    Defaults defaults_;

    struct TransparentStringHash {
        using is_transparent = void;
        usize operator()(std::string_view sv) const noexcept {
            return std::hash<std::string_view>{}(sv);
        }
        usize operator()(const std::string& s) const noexcept {
            return std::hash<std::string_view>{}(s);
        }
        usize operator()(const char* s) const noexcept {
            return std::hash<std::string_view>{}(s);
        }
    };
    std::unordered_map<std::string, gfx::TextureHandle, TransparentStringHash, std::equal_to<>>
        owned_;

};

} // namespace whiteout::flakes::renderer::assets
