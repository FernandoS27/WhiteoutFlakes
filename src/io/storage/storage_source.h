#pragma once

// ============================================================================
// One place content can come from: a CASC install, an MPQ archive.
//
// A source owns exactly one opened storage and everything about how that
// storage spells a path. It is deliberately narrow — the fallback order across
// several sources belongs to GameStorage, and the disk search path belongs to
// the provider, because neither is a property of a storage.
// ============================================================================

#include "whiteout/flakes/types.h"

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace whiteout::flakes::io {

// What a hit produced. `actualExt` is lowercase and includes the dot; it can
// differ from the extension that was asked for (a Reforged install answers a
// `.blp` request with a `.dds`), and consumers decode by it.
struct SourceRead {
    std::vector<u8> data;
    std::string actualExt;
};

class IStorageSource {
public:
    virtual ~IStorageSource() = default;

    // `path` is as the caller wrote it; each source normalises to its own
    // spelling. Returns false without touching `out` on a miss.
    virtual bool Read(const std::string& path, SourceRead& out) const = 0;

    // Only a source whose manifest is id-keyed overrides this. There is no
    // path to fall back to and no extension to probe, so a miss is a miss.
    virtual bool ReadById(u32 fileId, SourceRead& out) const {
        (void)fileId;
        (void)out;
        return false;
    }

    // Emits every entry in listing form (see ToListingPath). Sources that
    // cannot enumerate — a WoW root with no listfile — emit nothing.
    virtual void List(const std::function<void(std::string)>& emit) const = 0;

    // Where this source was opened from, for status display and diagnostics.
    virtual const std::string& Root() const = 0;
};

} // namespace whiteout::flakes::io
