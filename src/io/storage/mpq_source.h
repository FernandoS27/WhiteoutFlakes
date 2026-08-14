#pragma once

// ============================================================================
// One opened MPQ archive.
//
// Simpler than CASC in every way: paths are stored as the archive author
// typed them, there is no mod chain and no id manifest. Load order across
// several archives is GameStorage's business, not this one's.
// ============================================================================

#include "storage_source.h"

#if WHITEOUT_HAS_MPQ

#include <whiteout/storages/mpq/storage.h>

#include <memory>

namespace whiteout::flakes::io {

class MpqSource final : public IStorageSource {
public:
    // `file` is the archive's full path. Returns null when it will not open;
    // `error` says why.
    static std::unique_ptr<MpqSource> Open(std::string file, std::string& error);

    bool Read(const std::string& path, SourceRead& out) const override;
    void List(const std::function<void(std::string)>& emit) const override;
    const std::string& Root() const override {
        return file_;
    }

private:
    MpqSource(std::string file, whiteout::storages::mpq::Storage storage);

    std::string file_;
    whiteout::storages::mpq::Storage storage_;
};

} // namespace whiteout::flakes::io

#endif // WHITEOUT_HAS_MPQ
