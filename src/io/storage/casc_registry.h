#pragma once

// ============================================================================
// One opened CASC per install, shared by everything that reads it.
//
// Opening a CASC is expensive — index files, the encoding table and the root
// manifest are all parsed up front, which is seconds and hundreds of megabytes
// on a Reforged or retail install — and this process opens the same one from
// several places: every scene's content provider, the Storage Explorer, the
// model explorer's thumbnail grid. Opening it once per caller pays that cost
// once per caller and multiplies the resident set by the same factor, for a
// handle that is identical every time.
//
// So the open goes through here instead: same key, same `casc::Storage`.
// `casc::Storage`'s read API is const and takes a shared lock internally, so
// sharing one across threads needs nothing from callers beyond holding the
// handle. Everything that varies *per reader* rather than per install — the
// Warcraft III mod-prefix order, whether ids are read, the frame-suffix retry
// — stays on CascSource, which is why those are not part of the key.
//
// Entries are held weakly: the storage lives exactly as long as the handles do,
// so closing the last document that reads an install still frees it.
// ============================================================================

#include "whiteout/flakes/types.h"

#if WHITEOUT_HAS_CASC

#include <whiteout/storages/casc/storage.h>

#include <memory>
#include <string>
#include <vector>

namespace whiteout::utils {
class SimpleThreadPool;
}

namespace whiteout::flakes::io {

/// What an open depends on. Two requests that agree here get the same storage;
/// anything else is a different one, because it would read differently.
struct CascOpenKey {
    /// Install root, or its `Data/` subdirectory — `casc::Storage` accepts
    /// both. Normalised before it is compared, so two spellings of one path do
    /// not open it twice.
    std::string root;

    /// Community `id;path` CSV, loaded here because the storage borrows the
    /// bytes for its lifetime and a shared storage outlives any one caller's
    /// buffer.
    std::string listfilePath;

    /// Community `keyName keyHex` list, imported before the first read.
    std::string tactKeyFile;

    /// Zeros for a frame whose key is unknown rather than failing the file.
    bool zeroFillEncrypted = false;
};

/// An opened CASC plus everything it borrows, which is why they are one object:
/// the storage holds a non-owning pointer into the worker pool and a span over
/// the listfile bytes, and both have to outlive it.
class SharedCasc {
public:
    ~SharedCasc();
    SharedCasc(const SharedCasc&) = delete;
    SharedCasc& operator=(const SharedCasc&) = delete;

    const whiteout::storages::casc::Storage& Storage() const noexcept {
        return storage_;
    }
    /// The root the storage opened at, as the first caller spelled it — the
    /// key it is stored under is normalised, this is not.
    const std::string& Root() const noexcept {
        return root_;
    }
    bool HasListfile() const noexcept {
        return !listfile_.empty();
    }

private:
    friend std::shared_ptr<const SharedCasc> AcquireSharedCasc(const CascOpenKey&, std::string&);
    SharedCasc(std::string root, std::shared_ptr<whiteout::utils::SimpleThreadPool> pool,
               std::vector<u8> listfile, whiteout::storages::casc::Storage storage);

    std::string root_;
    // Both declared before `storage_`, which borrows them: members are
    // destroyed in reverse declaration order, so the storage goes first.
    std::shared_ptr<whiteout::utils::SimpleThreadPool> pool_;
    std::vector<u8> listfile_;
    whiteout::storages::casc::Storage storage_;
};

/// The storage for @p key, opening it only if nothing else already has it.
/// Null on failure, with @p error set. Concurrent callers asking for the same
/// key wait for the one open rather than racing to repeat it; callers asking
/// for different keys do not wait for each other.
std::shared_ptr<const SharedCasc> AcquireSharedCasc(const CascOpenKey& key, std::string& error);

/// How many installs are open right now. For tests and diagnostics — the point
/// of this file is that this number does not grow with the number of readers.
usize OpenCascCount();

} // namespace whiteout::flakes::io

#endif // WHITEOUT_HAS_CASC
