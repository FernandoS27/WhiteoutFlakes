#pragma once
// ============================================================================
// CornEffectsAssetCache — shared `cornflakes::EffectAssetModel` by .pkb path.
//
// Mirrors the engine's `pkSystem::LoadEmitter(filename)` cache (preview.exe
// 0x140412EF0): one parsed asset per .pkb path, shared across every active
// CornEffectsEmitter that references it. Backed by a single ExpandingArena that
// outlives every model + every EffectRuntime built off them.
//
// Thread-safety: Acquire() takes an internal mutex. Once an asset is in the
// cache subsequent Acquire() calls are read-only — the returned pointer is
// stable for the cache's lifetime, and EffectAssetModel is documented as
// safe to share read-only across multiple EffectRuntime instances.
//
// Lifetime contract: Clear() must run AFTER every EffectRuntime built from
// these models has been destroyed. CornEffectsService enforces this ordering.
// ============================================================================

#include "whiteout/flakes/types.h"

#include <cornflakes/interface/asset/asset_reader.hpp>
#include <cornflakes/interface/core/arena.hpp>

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace whiteout::cornflakes {
struct EffectAssetModel;
class  IssueBag;
}

namespace whiteout::flakes::io {
class IContentProvider;
}

namespace whiteout::flakes::renderer::corn_effects {

class CornEffectsAssetCache {
public:
    CornEffectsAssetCache();
    ~CornEffectsAssetCache();

    // Non-copyable, non-movable: callers hold pointers into the cache.
    CornEffectsAssetCache(const CornEffectsAssetCache&) = delete;
    CornEffectsAssetCache& operator=(const CornEffectsAssetCache&) = delete;
    CornEffectsAssetCache(CornEffectsAssetCache&&) = delete;
    CornEffectsAssetCache& operator=(CornEffectsAssetCache&&) = delete;

    void SetContentProvider(io::IContentProvider* provider);

    const ::whiteout::cornflakes::EffectAssetModel*
    Acquire(const std::string& pkbPath,
            ::whiteout::cornflakes::IssueBag& issues);

    ::whiteout::cornflakes::IArena& Arena() { return arena_; }

    void Clear();

private:
    struct Entry {
        std::vector<std::byte>                                          bytes;
        std::unique_ptr<::whiteout::cornflakes::EffectAssetModel>       model;
    };

    mutable std::mutex                                                  mutex_;
    ::whiteout::cornflakes::ExpandingArena                              arena_;
    ::whiteout::cornflakes::SerializerPriorityDispatcher                dispatcher_;
    std::unordered_map<std::string, std::unique_ptr<Entry>>             cache_;
    io::IContentProvider*                                               contentProvider_ = nullptr;
};

}
