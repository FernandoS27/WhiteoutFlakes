#pragma once

// ============================================================================
// AssetManager — push-based, slot-indirected asset registry.
//
// Replaces the current pull-based texture/.pkb/child-MDX caches. Renderer
// code declares its needs once via Acquire(); the host (JS / disk loader)
// drains the needs queue, fetches bytes its own way, and pushes them in
// via Apply(). Slots are refcounted across consumers and stable for the
// asset's lifetime — multiple actors/layers/templates that reference the
// same path share one slot.
//
// Threading: a single mutex protects the slot table. On WASM all calls
// land on the main thread so contention is zero. On desktop, Acquire and
// the CPU half of Apply (ApplyPrepared) can come from background loader
// threads; the GPU half (CommitPrepared) must run on the render thread.
// ============================================================================

#include "gfx/gfx.h"
#include "whiteout/flakes/types.h"

#include <cornflakes/interface/asset/asset_reader.hpp>
#include <cornflakes/interface/core/arena.hpp>

#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace whiteout::cornflakes {
struct EffectAssetModel;
}

namespace whiteout::flakes::renderer::model {
struct ModelTemplate;
}

namespace whiteout::flakes::renderer::assets {

class TextureAssetManager;

/// @brief Kinds of asset the manager tracks. Drives Apply()'s dispatch
///        and disambiguates which payload field a slot holds.
enum class AssetKind : u8 {
    Texture     = 0,
    Particle    = 1, ///< cornflakes .pkb / .pkfx
    ChildModel  = 2, ///< secondary .mdx referenced by attachments / PE1
};

class AssetManager {
public:
    using SlotId = u32;
    static constexpr SlotId kInvalidSlot = 0;

    /// @brief Fired (deferred via the needs queue, see DrainNeeds) the
    ///        first time a path is Acquired. The host uses this to
    ///        decide what to fetch.
    using NeededFn = std::function<void(AssetKind, std::string_view path)>;

    explicit AssetManager(TextureAssetManager& textures);
    ~AssetManager();

    AssetManager(const AssetManager&)            = delete;
    AssetManager& operator=(const AssetManager&) = delete;

    // ── Renderer side ────────────────────────────────────────────────────

    /// @brief Reserve a slot for @p path. Refcount is incremented if the
    ///        slot already exists; otherwise a fresh slot is allocated
    ///        with a placeholder payload and the path is queued onto the
    ///        needs list for the host to fetch.
    SlotId Acquire(AssetKind kind, std::string_view path);

    /// @brief Decrement refcount. When it hits zero the slot is freed
    ///        and any held GPU resources are scheduled for destruction.
    void Release(SlotId slot);

    /// @brief True once a successful Apply has populated the payload.
    bool Loaded(SlotId slot) const;

    /// @brief Path-keyed predicate: true iff a Texture slot for @p path
    ///        exists AND its payload has arrived (Apply has run). Used
    ///        by the Max plugin's live adapter for cross-model dedup —
    ///        skip BLP/CASC decode when another model already uploaded
    ///        the same texture.
    bool IsTextureCached(std::string_view path) const;

    /// @brief Monotonic counter bumped each time a slot's payload swaps.
    ///        Consumers cache the last-seen generation to detect changes
    ///        without comparing handles.
    u32 GenerationOf(SlotId slot) const;

    /// @brief Current GPU texture for a Texture slot. Returns the shared
    ///        placeholder while the real bytes haven't arrived (or if
    ///        the slot is the wrong kind).
    gfx::TextureHandle TextureOf(SlotId slot) const;

    /// @brief Parsed PopcornFX asset model. Null until Apply has run.
    const cornflakes::EffectAssetModel* ParticleAssetOf(SlotId slot) const;

    /// @brief Built child-model template. Null until Apply has run.
    std::shared_ptr<model::ModelTemplate> ChildModelOf(SlotId slot) const;

    // ── Host side ────────────────────────────────────────────────────────

    /// @brief Drain the buffered "I need this" queue. The callback fires
    ///        once per unique path that was Acquired since the previous
    ///        drain. Called after each renderer entry point returns so
    ///        the host can issue fetches without the renderer being
    ///        mid-call (avoids re-entry).
    void DrainNeeds(const NeededFn& cb);

    /// @brief Number of paths currently queued in the needs list.
    std::size_t PendingNeedsCount() const;

    /// @brief CPU half of Apply: decode/parse @p bytes for the slot
    ///        currently bound to @p path and stash the result in the
    ///        prepared queue. Texture bytes are decoded to gfx pixel
    ///        data + mip chain; Particle bytes are parsed into an
    ///        EffectAssetModel inside a per-slot arena; ChildModel
    ///        bytes are handed to the host-provided builder for MDX
    ///        parse. Returns true iff a slot exists for @p path AND
    ///        the decode succeeded.
    bool ApplyPrepared(AssetKind kind, std::string_view path,
                       std::span<const u8> bytes, std::string_view foundExt = {});

    /// @brief GPU half of Apply: drains the prepared queue and finalises
    ///        each entry against its slot — creates GPU textures, swaps
    ///        the slot's payload pointer, bumps generation, frees the
    ///        old payload. Must be called from the render thread; the
    ///        FrameTicker invokes it once per frame.
    void CommitPrepared();

    /// @brief Provide the GPU device the manager uses for texture
    ///        upload + destruction. Called by RenderPipeline once the
    ///        device exists; AssetManager itself is constructed earlier
    ///        so it can be referenced by other subsystems.
    void SetGfxDevice(gfx::IGFXDevice* gfx);

    /// @brief Builder for the ChildModel kind. AssetManager itself
    ///        doesn't know how to parse MDX; RenderService installs a
    ///        builder that wraps ModelTemplateManager's parse path
    ///        (the same path SpawnUnit's GetOrLoadSync uses for the
    ///        top-level MDX). Called from ApplyPrepared(ChildModel)
    ///        with the pre-fetched bytes — return nullptr to signal
    ///        parse failure.
    using ChildModelBuilder = std::function<
        std::shared_ptr<model::ModelTemplate>(
            std::string_view path, std::span<const u8> bytes, std::string_view foundExt)>;
    void SetChildModelBuilder(ChildModelBuilder builder);

    /// @brief Fires after a slot's payload is swapped in by CommitPrepared,
    ///        outside the manager mutex so the callback can Acquire other
    ///        slots safely. Hosts use this to scan a freshly-applied asset
    ///        for secondary references (e.g. corn-fx layer textures) and
    ///        eagerly Acquire them — see AddDependency for the lifetime
    ///        tie-in.
    using OnAppliedFn = std::function<void(SlotId, AssetKind)>;
    void SetOnApplied(OnAppliedFn cb);

    /// @brief Register @p child as a dependency of @p parent. When the
    ///        parent's refcount reaches zero, every dependency is
    ///        released automatically. Use this from an OnApplied hook
    ///        to tie a parent slot's lifetime to slots it transitively
    ///        references. No-op if either id is invalid or @p parent
    ///        no longer exists.
    void AddDependency(SlotId parent, SlotId child);

    // ── Diagnostics ──────────────────────────────────────────────────────
    struct Stats {
        std::size_t liveSlots          = 0;
        std::size_t loadedSlots        = 0;
        std::size_t totalAcquires      = 0;
        std::size_t totalReleases      = 0;
        std::size_t totalApplies       = 0;
        std::size_t totalApplyMisses   = 0; ///< Apply called with no matching slot
    };
    Stats GetStats() const;

private:
    struct Slot {
        AssetKind kind;
        std::string path;
        u32       refCount  = 0;
        u32       generation = 0;
        bool      loaded    = false;

        // Texture
        gfx::TextureHandle texHandle = gfx::TextureHandle::Invalid;
        // Particle / ChildModel: owned via shared_ptr so older snapshots
        // held by consumers remain valid after a swap (single-writer,
        // many-reader). Slot-swap is just pointer assignment under the
        // mutex.
        std::shared_ptr<const cornflakes::EffectAssetModel> particleAsset;
        // PkbReader keeps spans pointing into both the source byte
        // buffer AND a parse-time arena. Both must live as long as
        // the slot does, and — critically — must FREE when the slot
        // dies so memory reclaims on model unload. A per-slot arena
        // makes that automatic: the unique_ptr destructor releases
        // the arena's chunks on Release-to-zero.
        std::shared_ptr<std::vector<std::byte>> particleBytes;
        std::unique_ptr<cornflakes::ExpandingArena> particleArena;
        std::shared_ptr<model::ModelTemplate> childTemplate;
        // Slots that should be released when this slot's refcount drops
        // to zero — used by the OnApplied hook to tie texture slots to
        // the parent particle/child-model slot.
        std::vector<SlotId> dependencies;
    };

    struct Prepared {
        SlotId slot;
        AssetKind kind;
        // Texture half-decoded payload — bytes are decoded in
        // ApplyPrepared (CPU); CommitPrepared turns the buffer into
        // the GPU texture (render thread). `pixels` holds every mip
        // level concatenated in order; the gfx upload path walks
        // them based on (format, w, h, mipLevels).
        std::vector<u8> pixels;
        i32 width     = 0;
        i32 height    = 0;
        i32 mipLevels = 1;
        gfx::Format format = gfx::Format::Unknown;
        // Particle / ChildModel: already-parsed payload, ready to assign.
        std::shared_ptr<const cornflakes::EffectAssetModel> particleAsset;
        std::shared_ptr<std::vector<std::byte>> particleBytes;
        std::unique_ptr<cornflakes::ExpandingArena> particleArena;
        std::shared_ptr<model::ModelTemplate> childTemplate;
    };

    SlotId AllocSlotId() noexcept;
    static std::string Normalize(std::string_view in);

    mutable std::mutex mu_;
    std::unordered_map<std::string, SlotId> pathToSlot_;
    std::unordered_map<SlotId, Slot> slots_;
    std::deque<std::pair<AssetKind, std::string>> needs_;
    std::deque<Prepared> prepared_;
    SlotId nextSlot_ = 1;

    TextureAssetManager& textures_; // for the placeholder white handle
    gfx::IGFXDevice* gfx_ = nullptr;

    // Particle parsing dispatcher (PkbReader for .pkb / .pkfx). The
    // arena that backs each parsed EffectAssetModel lives ON the slot
    // (see `Slot::particleArena`), so memory is reclaimed when the
    // slot is released. The dispatcher itself is stateless across
    // calls — only the arena and source bytes vary.
    cornflakes::SerializerPriorityDispatcher particleDispatch_;
    ChildModelBuilder childModelBuilder_;
    OnAppliedFn onApplied_;

    // Stats (under mu_).
    std::size_t statAcquires_      = 0;
    std::size_t statReleases_      = 0;
    std::size_t statApplies_       = 0;
    std::size_t statApplyMisses_   = 0;
};

} // namespace whiteout::flakes::renderer::assets
