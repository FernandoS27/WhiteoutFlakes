#pragma once

// ============================================================================
// One opened CASC install.
//
// The three games differ here more than anywhere else, so the differences are
// options rather than branches spread through a read:
//
//   Warcraft III  a TVFS mod chain (`war3.w3mod:` / `_hd.w3mod:`) that every
//                 path is tried under, in an order the HD toggle flips, plus
//                 the Reforged frame-suffix rename.
//   World of Warcraft  bare paths, but the root is keyed by fileDataID and
//                 carries no names at all without a community listfile.
//   StarCraft II / Heroes  readable names, but every asset lives under a mod
//                 root (`mods/*.sc2mod/base.sc2assets/`, campaign variants)
//                 while an `.m3` names its textures relative to that root
//                 (`assets/textures/...`) — so relative asset reads retry
//                 under prefixes learned from the storage's own listing.
//
// A source built without an option does not pay for it: no mod chain means
// one probe per extension instead of four.
// ============================================================================

#include "casc_registry.h"
#include "storage_source.h"

#if WHITEOUT_HAS_CASC

#include <whiteout/storages/casc/storage.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace whiteout::flakes::io {

struct CascSourceOptions {
    // Warcraft III's mod-prefix chain. Non-null enables it, and the flag it
    // points at picks the order: HD first when set, so `_hd.w3mod` overrides
    // win — the same dance as Reforged's W3Data::OpenMod. Every other product
    // stores bare paths and leaves this null.
    //
    // A pointer rather than a copy because the host flips HD mode at runtime
    // and a reopen for that would be absurd.
    const std::atomic<bool>* hdMode = nullptr;

    // Reads by fileDataID (World of Warcraft). Costs nothing until an id is
    // actually asked for.
    bool fileIds = false;

    // Reforged dropped the classic "-<frame>" suffix on some stock texture
    // sets — Textures\Water07-0.blp ships as textures\water07.dds. On a miss,
    // retry with a trailing -<digits> stripped. Warcraft III only: elsewhere
    // it would be a wrong extra probe on every miss.
    bool frameSuffixFallback = false;

    // Community `id;path` CSV. A path rather than the bytes: the storage
    // borrows them for its lifetime and is shared between readers, so the
    // registry owns them (see casc_registry.h).
    std::string listfilePath;

    // Community `keyName keyHex` list, read at open. Blizzard encrypts frames
    // of shipped files with per-content TACT keys, and a file with one such
    // frame reads as *missing* without the key — the encoding is inside the
    // container, so there is no error to distinguish it from an absent file.
    std::string tactKeyFile;

    // Substitute zeros for a frame whose key is still unknown rather than
    // failing the whole read. Unreleased content ships with keys nobody has
    // published, and one such frame otherwise costs the entire file.
    bool zeroFillEncrypted = false;

    // StarCraft II / Heroes: retry a missed `assets/...` path under every
    // `<mod root>/assets/` prefix the storage's listing carries, most-derived
    // mod first — the same shadowing the game's dependency chain resolves.
    // The prefix list is built lazily from one enumeration, on the first miss
    // that needs it.
    bool assetPrefixFallback = false;
};

class CascSource final : public IStorageSource {
public:
    // Returns null when the root holds no storage; `error` says why.
    static std::unique_ptr<CascSource> Open(std::string root, const CascSourceOptions& opts,
                                            std::string& error);

    bool Read(const std::string& path, SourceRead& out) const override;
    bool ReadById(u32 fileId, SourceRead& out) const override;
    u32 FileIdForPath(const std::string& path) const override;
    std::string PathForFileId(u32 fileId) const override;
    void List(const std::function<void(std::string)>& emit) const override;
    const std::string& Root() const override {
        return shared_->Root();
    }
    bool HasListfile() const {
        return shared_->HasListfile();
    }

private:
    CascSource(std::shared_ptr<const SharedCasc> shared, const CascSourceOptions& opts);

    // One stem, every prefix and extension this source knows. True on a hit.
    bool ReadStem(const std::string& stem, const std::string& ext, SourceRead& out) const;

    // One prefix, the asked extension then its alternates. True on a hit.
    bool ReadPrefixed(const std::string& prefix, const std::string& stem, const std::string& ext,
                      SourceRead& out) const;

    // The learned `<mod root>/` prefixes, built on first use (one enumeration
    // of the storage), ordered most-derived mod first.
    const std::vector<std::string>& AssetPrefixes() const;

    const whiteout::storages::casc::Storage& storage_() const noexcept {
        return shared_->Storage();
    }

    // Shared with every other reader of the same install, and const throughout
    // — `casc::Storage`'s read API takes its own shared lock, so concurrent
    // reads through one handle need nothing here.
    std::shared_ptr<const SharedCasc> shared_;
    const std::atomic<bool>* hdMode_ = nullptr;
    bool fileIds_ = false;
    bool frameSuffixFallback_ = false;
    bool assetPrefixFallback_ = false;
    mutable std::mutex assetPrefixMu_;
    mutable bool assetPrefixesBuilt_ = false;
    mutable std::vector<std::string> assetPrefixes_;
};

} // namespace whiteout::flakes::io

#endif // WHITEOUT_HAS_CASC
