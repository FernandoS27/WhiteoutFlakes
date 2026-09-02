#pragma once

// ============================================================================
// D3ItemRegistry — every item the client can name, ready to equip.
//
// "Equip by in-game name" needs the GameBalance Items tables, and a 2.8
// client ships exactly five of them (3,686 records, 3,491 with art — see
// Corpus/D3/GameBalance/README.md). This registry parses them once, computes
// each record's gbid (the disk field is zeroed; the engine stamps
// Str_HashLower33(szName) at load, and so does this), and reads each item's
// Actor once to classify where the item can go:
//
//   armour slot      <=>  the Actor carries tag 0x10400 (a look value)
//   attachment slot  <=>  the hardpoint rule yields a name for that slot
//
// The type NAMES are reconstructed from `gbidItemType` by hashing a table of
// known names (the exe's own display table at 0x1447C00 plus the record
// spellings that crack against shipped data); the records themselves are
// server-side in 2.8 and cannot be read. An item whose type does not crack
// still equips — classification degrades to what its Actor's tags say.
//
// Built lazily on first use: 5 table reads plus ~3.4k small Actor reads,
// once per storage. Two sources, tried in order: the provider's CASC
// (ListFiles over Base/GameBalance), then a plain directory of `.gam` files
// (the corpus snapshot) for offline use.
//
// The build is self-contained on purpose — it parses its own bytes rather
// than going through the shared D3SnoCache — so a host can run EnsureBuilt
// on a task thread while the render thread keeps using the cache. The Actor
// reads are the whole cost, and they are batched through the provider's
// request queue: the worker pool runs them concurrently, where the one-
// ReadFile-at-a-time shape paid up to a frame of Pump latency per file.
// ============================================================================

#include "whiteout/flakes/types.h"

#include <whiteout/sno/d3/native/character.h>
#include <whiteout/sno/d3/native/types.h>

#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace whiteout::flakes::io {

class IContentProvider;
class ProgressMonitor;

namespace d3n = ::whiteout::sno::d3::native;

/// Every class can wear it — one bit per `PlayerClass` ordinal.
inline constexpr u8 kD3AllClasses = 0x7F;

/// One GameBalance item record plus what its Actor says about wearing it.
struct D3ItemRecord {
    std::string name;        ///< `szName`, the in-game identity.
    std::string displayName; ///< Items.stl value ("Goldskin"); empty = unnamed.
    u32 gbid = 0;            ///< gbidHash(name) — the engine's key for it.
    i32 snoActor = -1;       ///< The item's art; -1 for the 195 art-less records.
    u32 gbidItemType = 0;    ///< Hash of the ItemType name.
    u32 gbidSet = 0xFFFFFFFFu; ///< gbidHash of the ItemSets.stl key; -1 = no set.
    /// Who can wear it: type-derived, then set-inherited (a set with one
    /// class-typed member is that class throughout), then the curated
    /// all-generic-typed sets. `kD3AllClasses` = genuinely neutral.
    u8 classMask = kD3AllClasses;

    // Read off the Actor's tag map at build time (absent when snoActor is -1
    // or the Actor did not resolve).
    std::optional<u32> lookValue;    ///< tag 0x10400 — an armour piece.
    std::optional<u32> lookNameHash; ///< tag 0x10401.
    bool hasHoldType = false;        ///< tag 0x10081 — held like a weapon.
    bool hasPerClassArt = false;     ///< tag 0x17020.
    u32 hairStyle = 0;               ///< tag 0x10404 — a helm's hair cutaway.

    /// Bit per `EVisualSlot` 0..7 this item is offered under.
    u16 slotMask = 0;

    bool CanGo(d3n::EVisualSlot slot) const {
        return (slotMask >> static_cast<u32>(slot)) & 1u;
    }
    bool CanWear(d3n::PlayerClass cls) const {
        return (classMask >> static_cast<u32>(cls)) & 1u;
    }
};

/// An item set, joined from the records' `gbidSet` against ItemSets.stl.
struct D3ItemSet {
    std::string key;         ///< The StringList key ("Ninja_Set_x1").
    std::string displayName; ///< "The Shadow's Mantle". Empty without the STL.
    u32 gbid = 0;            ///< gbidHash(key) — what the records carry.
    u8 classMask = kD3AllClasses;
    std::vector<u32> members; ///< Indices into Items(), name-sorted.
};

class D3ItemRegistry {
public:
    D3ItemRegistry() = default;

    /// @brief A directory of `.gam` files to fall back on when the provider
    ///        has no GameBalance (an MPQ-era build, or no install at all).
    void SetFallbackDirectory(std::filesystem::path dir) {
        fallbackDir_ = std::move(dir);
    }

    /// @brief A directory of `.acr` files to classify against when the
    ///        provider cannot answer an id read (the corpus tree is
    ///        name-keyed on disk and id-keyed inside, so this scans the file
    ///        headers once to build the id -> path map).
    void SetFallbackActorDirectory(std::filesystem::path dir) {
        fallbackActorDir_ = std::move(dir);
    }

    /// @brief A directory of `.stl` files (the Corpus/D3/StringList snapshot)
    ///        for display names when the provider has none to offer.
    void SetFallbackStringListDirectory(std::filesystem::path dir) {
        fallbackStlDir_ = std::move(dir);
    }

    /// @brief Parse the tables and classify, once. Safe to call every time
    ///        the dressing room opens; only the first call pays. Runs on
    ///        whatever thread calls it — a task thread works, and @p progress
    ///        (optional) is how it reports there. A cancelled build clears
    ///        itself so the next call retries; any other outcome latches.
    /// @returns true when at least one item record exists afterwards.
    bool EnsureBuilt(IContentProvider* provider, ProgressMonitor* progress = nullptr);

    bool Built() const {
        return built_;
    }

    /// @brief Forget everything — a different storage means different items.
    void Clear();

    std::span<const D3ItemRecord> Items() const {
        return items_;
    }

    /// @brief The record named @p name, case-insensitively, or null.
    const D3ItemRecord* FindByName(std::string_view name) const;

    /// @brief The record whose gbid is @p gbid, or null.
    const D3ItemRecord* FindByGbid(u32 gbid) const;

    /// @brief Indices into Items() offered for @p slot, sorted by name.
    std::vector<u32> ItemsForSlot(d3n::EVisualSlot slot) const;

    /// @brief Every set with at least one member, sorted by display name.
    std::span<const D3ItemSet> Sets() const {
        return sets_;
    }

    /// @brief The set @p rec belongs to, or null.
    const D3ItemSet* SetOf(const D3ItemRecord& rec) const;

    /// @brief The cracked ItemType name for @p gbidItemType, or empty. Static:
    ///        the table is data about the hash space, not about a storage.
    static std::string_view ItemTypeName(u32 gbidItemType);

    /// @brief Like ItemTypeName, but consulting the loaded ItemTypeNames.stl
    ///        keys first — those close the class-suffixed armour types the
    ///        builtin table cannot know.
    std::string_view TypeNameOf(u32 gbidItemType) const;

private:
    /// What classification needs from one Actor — extracted at parse time so
    /// the parsed tree (materials, hardpoints, everything) is dropped
    /// immediately instead of ~3.4k of them living to the end of the build.
    struct ActorTags {
        std::optional<u32> lookValue;
        std::optional<u32> lookNameHash;
        bool hasHoldType = false;
        bool hasPerClassArt = false;
        u32 hairStyle = 0;
    };

    static ActorTags TagsOf(const d3n::Actor& actor);
    bool LoadTables(IContentProvider* provider, ProgressMonitor* progress);
    void LoadStringLists(IContentProvider* provider);
    void Classify(IContentProvider* provider, ProgressMonitor* progress);
    void BuildSets();
    /// The tags of every Actor in @p ids: batched through the provider's
    /// request queue, then the fallback directory's header index for what the
    /// provider could not answer. Ids neither can serve are simply absent.
    std::unordered_map<i32, ActorTags> ReadActorTags(IContentProvider* provider,
                                                     const std::vector<i32>& ids,
                                                     ProgressMonitor* progress);

    std::filesystem::path fallbackDir_;
    std::filesystem::path fallbackActorDir_;
    std::filesystem::path fallbackStlDir_;
    bool actorIndexBuilt_ = false;
    std::unordered_map<i32, std::filesystem::path> actorPathById_;
    bool built_ = false;
    std::vector<D3ItemRecord> items_;
    std::unordered_map<u32, u32> byGbid_; ///< gbid -> index into items_.
    std::vector<D3ItemSet> sets_;
    std::unordered_map<u32, u32> setByGbid_; ///< set gbid -> index into sets_.
    /// gbidHash(ItemTypeNames.stl key) -> the key spelling. Values are
    /// node-stable, so TypeNameOf can hand out views into them.
    std::unordered_map<u32, std::string> typeNames_;
};

} // namespace whiteout::flakes::io
