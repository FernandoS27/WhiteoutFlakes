#pragma once

// ============================================================================
// D3AssetProvider — WhiteoutLib's native::AssetProvider over IContentProvider.
//
// Diablo III's CASC root is id-keyed and a SNO id *is* the file id, so a load
// is `ReadFile(ContentRef::FromFileId(snoId))` and nothing else. No listfile,
// no path retry, no group in the key — ids are globally unique across groups
// (measured: 4051 files across 11 groups, 4051 distinct header snoIds, zero
// collisions), which is also why `Group` is accepted and ignored here.
//
// The memo is load-scoped and must stay that way. A graph walk can reach one
// asset twice — two SubObjects naming one `.mat`, an AnimSet whose base is
// itself — so a per-load memo removes the duplicate CASC read; keeping a 4.5 MB
// `.app` byte buffer alive next to its parsed form would double the cost of the
// thing D3SnoCache is caching, for a buffer nothing reads again. So this dies
// with the load, and the *parsed* cache is what survives it.
//
// `native::AssetProvider::load` returns `std::vector<u8>` **by value**. That is
// WhiteoutLib's signature and the one place this seam costs a copy; the copy is
// bounded by D3SnoCache keeping the call from happening at all on a hit, so it
// is noted rather than worked around. If it ever shows up in a profile the fix
// is a span-returning overload upstream, not a shim here.
// ============================================================================

#include "whiteout/flakes/content_provider.h"
#include "whiteout/flakes/types.h"

#include <whiteout/sno/d3/native/d3_native.h>

#include <memory>
#include <unordered_map>
#include <vector>

namespace whiteout::flakes::io {

namespace d3n = ::whiteout::sno::d3::native;

/// @brief Read SNO bytes by id, memoised for the duration of one load.
class D3AssetProvider final : public d3n::AssetProvider {
public:
    explicit D3AssetProvider(IContentProvider* provider) : provider_(provider) {}

    std::vector<::whiteout::u8> load(d3n::Group group, ::whiteout::i32 snoId) override;

    /// @brief The same read without the by-value copy, for callers that walk
    ///        the graph themselves (which is every renderer path — see
    ///        D3ModelAdapter). Returns null when the id is unset or missing.
    std::shared_ptr<const std::vector<::whiteout::u8>> Read(::whiteout::i32 snoId);

    /// @brief Drop the memo. The parsed cache outlives a load; this does not.
    void Reset() {
        memo_.clear();
    }

    usize Reads() const {
        return reads_;
    }
    usize MemoHits() const {
        return memoHits_;
    }

private:
    IContentProvider* provider_ = nullptr;
    std::unordered_map<::whiteout::i32, std::shared_ptr<const std::vector<::whiteout::u8>>> memo_;
    usize reads_ = 0;
    usize memoHits_ = 0;
};

} // namespace whiteout::flakes::io
