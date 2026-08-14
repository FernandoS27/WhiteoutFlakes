#pragma once

// ============================================================================
// The set of storages one game reads from, and the builder that assembles it.
//
// A GameStorage is immutable once built: changing what should be open means
// building a new one. That is what makes reconfiguration a value-level
// operation (see FileContentProvider's deferred rebuild) instead of a
// sequence of reopen calls that can half-fail.
//
// Sources are searched in the order they were added, and each one answers
// fully before the next is asked.
// ============================================================================

#include "storage_source.h"
#include "whiteout/flakes/enums.h" // ProductId

#include <atomic>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace whiteout::utils {
class SimpleThreadPool;
}

namespace whiteout::flakes::io {

class GameStorage {
public:
    ~GameStorage();
    GameStorage(const GameStorage&) = delete;
    GameStorage& operator=(const GameStorage&) = delete;

    ProductId Game() const {
        return game_;
    }

    // First hit wins. False leaves `out` untouched.
    bool Read(const std::string& path, SourceRead& out) const;
    bool ReadById(u32 fileId, SourceRead& out) const;
    void List(const std::function<void(std::string)>& emit) const;

    bool HasCasc() const {
        return cascCount_ > 0;
    }
    bool HasArchives() const {
        return sources_.size() > cascCount_;
    }
    // Roots of the CASC installs that actually opened, in search order. Two
    // are offered for StarCraft II / Heroes and either can fail on its own,
    // which a bare HasCasc() cannot express.
    std::vector<std::string> CascRoots() const;

    // True when a listfile was configured and read. A World of Warcraft root
    // is id-keyed and carries no names, so this is the difference between a
    // storage that can be browsed and one that can only be read by id.
    bool HasListfile() const {
        return !listfile_.empty();
    }

private:
    friend class StorageBuilder;
    explicit GameStorage(ProductId game, std::vector<u8> listfile);

    ProductId game_;
    // Held for the storages' lifetime: casc::Storage borrows the bytes rather
    // than copying them. Declared before `sources_` so it outlives them.
    std::vector<u8> listfile_;
    std::vector<std::unique_ptr<IStorageSource>> sources_;
    // CASC sources lead, so this also splits `sources_` into its two halves.
    usize cascCount_ = 0;
};

// Assembles a GameStorage a piece at a time. Each game's rules are one
// function that calls only the pieces that game has (see game_rules.h) — a
// product with no archives never mentions archives, and pays for none.
class StorageBuilder {
public:
    explicit StorageBuilder(ProductId game) : game_(game) {}

    // Shared by every CASC source built here. CASC keeps a non-owning pointer,
    // so the pool must outlive the GameStorage.
    StorageBuilder& Pool(whiteout::utils::SimpleThreadPool* pool);

    // Warcraft III's TVFS mod chain, ordered by the flag `hdMode` points at.
    // Only this product has one.
    StorageBuilder& ModChain(const std::atomic<bool>* hdMode);

    // Read by fileDataID. World of Warcraft's root is keyed that way.
    StorageBuilder& FileIds();

    // Retry a texture miss with a trailing "-<digits>" stripped, for the
    // Reforged rename of the classic frame-suffixed sets.
    StorageBuilder& FrameSuffixFallback();

    // Community `id;path` CSV that makes an id-keyed root browsable. Read at
    // Build() time; a path that does not exist is reported and ignored.
    StorageBuilder& Listfile(std::string csvPath);

    // A CASC install root. Empty is skipped, so callers can pass a path that
    // may not have been discovered. Add order is search order.
    StorageBuilder& Casc(std::string root);

    // MPQ archives, named relative to `installRoot`, in load order. A name
    // that is not on disk is reported and skipped — WoW's archive set varies
    // by expansion, so a missing one is expected rather than an error.
    StorageBuilder& Archives(const std::string& installRoot, const std::vector<std::string>& names);

    std::unique_ptr<GameStorage> Build();

private:
    ProductId game_;
    whiteout::utils::SimpleThreadPool* pool_ = nullptr;
    const std::atomic<bool>* hdMode_ = nullptr;
    bool fileIds_ = false;
    bool frameSuffixFallback_ = false;
    std::string listfilePath_;
    std::vector<std::string> cascRoots_;
    std::vector<std::string> archiveFiles_;
};

} // namespace whiteout::flakes::io
