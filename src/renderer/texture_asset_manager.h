#pragma once

#include "../gfx/gfx.h"
#include "common_types.h"
#include "sampler_asset_manager.h"

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace WhiteoutDex {

class TextureAssetManager {
public:
    explicit TextureAssetManager(gfx::IGFXDevice& gfx);
    ~TextureAssetManager();

    TextureAssetManager(const TextureAssetManager&)            = delete;
    TextureAssetManager& operator=(const TextureAssetManager&) = delete;

    gfx::TextureHandle AcquireShared(std::string_view        key,
                                     const gfx::TextureDesc& desc,
                                     const void*             pixels);
    void               ReleaseShared(std::string_view key);

    bool IsCachedShared(std::string_view key) const;

    gfx::TextureHandle TryAcquireShared(std::string_view key);

    struct SharedStats {
        usize uniqueEntries = 0;
        usize totalAcquires = 0;
        usize cacheHits     = 0;
    };
    SharedStats GetSharedStats() const;
    void        ResetSharedStats();

    class ModelScope {
    public:

        gfx::TextureHandle Upload(i32                     textureId,
                                  const gfx::TextureDesc& desc,
                                  const void*             pixels,
                                  u32                     wrapFlags);

        gfx::TextureHandle UploadShared(i32                     textureId,
                                        std::string_view        sharedKey,
                                        const gfx::TextureDesc& desc,
                                        const void*             pixels,
                                        u32                     wrapFlags);

        gfx::TextureHandle BindShared(i32              textureId,
                                      std::string_view sharedKey,
                                      u32              wrapFlags);

        gfx::TextureHandle Get(i32 textureId) const noexcept;

        u32 WrapFlags(i32 textureId) const noexcept;

        usize Size() const noexcept { return entries_.size(); }

        void Clear();

        ~ModelScope();
        ModelScope(const ModelScope&)            = delete;
        ModelScope& operator=(const ModelScope&) = delete;

    private:
        friend class TextureAssetManager;
        ModelScope(gfx::IGFXDevice& gfx, TextureAssetManager& mgr)
            : gfx_(gfx), mgr_(mgr) {}

        void DropEntry(i32 textureId);

        gfx::IGFXDevice&     gfx_;
        TextureAssetManager& mgr_;
        struct Entry {
            gfx::TextureHandle tex       = gfx::TextureHandle::Invalid;
            u32                wrapFlags = kSamplerWrapBitsMask;

            std::string        sharedKey;
        };
        std::unordered_map<i32, Entry> entries_;
    };

    std::unique_ptr<ModelScope> CreateModelScope();

    void RegisterOwned(std::string name, gfx::TextureHandle handle);
    void ReleaseOwned(std::string_view name);
    gfx::TextureHandle GetOwned(std::string_view name) const noexcept;

    struct DebugEntry {
        std::string        name;
        gfx::TextureHandle handle;
    };
    std::vector<DebugEntry> DebugSnapshotOwned() const;

    struct Defaults {
        gfx::TextureHandle White       = gfx::TextureHandle::Invalid;
        gfx::TextureHandle Black       = gfx::TextureHandle::Invalid;
        gfx::TextureHandle FlatNormal  = gfx::TextureHandle::Invalid;
        gfx::TextureHandle NeutralOrm  = gfx::TextureHandle::Invalid;
        gfx::TextureHandle Missing     = gfx::TextureHandle::Invalid;
    };
    const Defaults& GetDefaults() const noexcept { return defaults_; }

private:
    gfx::IGFXDevice& gfx_;
    Defaults         defaults_;

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
    std::unordered_map<std::string, gfx::TextureHandle,
                       TransparentStringHash, std::equal_to<>> owned_;

    struct SharedEntry {
        gfx::TextureHandle handle   = gfx::TextureHandle::Invalid;
        usize              refCount = 0;
    };
    std::unordered_map<std::string, SharedEntry,
                       TransparentStringHash, std::equal_to<>> shared_;

    mutable std::mutex sharedMutex_;

    usize sharedTotalAcquires_ = 0;
    usize sharedCacheHits_     = 0;
};

}
