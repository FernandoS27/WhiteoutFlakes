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
//   StarCraft II / Heroes  bare paths, readable names, nothing else.
//
// A source built without an option does not pay for it: no mod chain means
// one probe per extension instead of four.
// ============================================================================

#include "storage_source.h"

#if WHITEOUT_HAS_CASC

#include <whiteout/storages/casc/storage.h>

#include <atomic>
#include <memory>
#include <span>

namespace whiteout::utils {
class SimpleThreadPool;
}

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

    // Community `id;path` CSV. The storage borrows it for its lifetime, so the
    // bytes belong to whoever owns the source (see GameStorage).
    std::span<const u8> listfile;

    // Community `keyName keyHex` list, read at open. Blizzard encrypts frames
    // of shipped files with per-content TACT keys, and a file with one such
    // frame reads as *missing* without the key — the encoding is inside the
    // container, so there is no error to distinguish it from an absent file.
    std::string tactKeyFile;

    // Substitute zeros for a frame whose key is still unknown rather than
    // failing the whole read. Unreleased content ships with keys nobody has
    // published, and one such frame otherwise costs the entire file.
    bool zeroFillEncrypted = false;

    // Shared with every other CASC source in the same storage set. CASC
    // parallelises index/encoding-table parsing and BLTE decompression across
    // it, and keeps a non-owning pointer — so the pool must outlive the source.
    whiteout::utils::SimpleThreadPool* pool = nullptr;
};

class CascSource final : public IStorageSource {
public:
    // Returns null when the root holds no storage; `error` says why.
    static std::unique_ptr<CascSource> Open(std::string root, const CascSourceOptions& opts,
                                            std::string& error);

    bool Read(const std::string& path, SourceRead& out) const override;
    bool ReadById(u32 fileId, SourceRead& out) const override;
    u32 FileIdForPath(const std::string& path) const override;
    void List(const std::function<void(std::string)>& emit) const override;
    const std::string& Root() const override {
        return root_;
    }

private:
    CascSource(std::string root, whiteout::storages::casc::Storage storage,
               const CascSourceOptions& opts);

    // One stem, every prefix and extension this source knows. True on a hit.
    bool ReadStem(const std::string& stem, const std::string& ext, SourceRead& out) const;

    std::string root_;
    whiteout::storages::casc::Storage storage_;
    const std::atomic<bool>* hdMode_ = nullptr;
    bool fileIds_ = false;
    bool frameSuffixFallback_ = false;
};

} // namespace whiteout::flakes::io

#endif // WHITEOUT_HAS_CASC
