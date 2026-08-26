#pragma once

// ============================================================================
// ParticleColor → the colours a creature `.m2` deliberately leaves to the game.
//
// A skin does not only fill texture slots. `CreatureDisplayInfo` also names a
// `ParticleColor` row, and `ReplaceParticleColor` (6.0.1 @0x100345e20, still
// there in 11.x) pushes that row's colours into every particle emitter whose
// `particleColorIndex` claims the matching slot — so two displays of the same
// model can breathe fire and frost out of one set of emitters.
//
// The row is three colours for each of three slots, and the slot numbering is
// shared with the texture side: `particleColorIndex` 11/12/13 is
// `TextureVariation[0..2]`'s numbering, not a coincidence. An emitter tagged 12
// is saying "tint me with whatever the skin put in slot 1".
//
// Three rules the client enforces that this table's callers have to keep, all
// measured and cited in M2_SKIN_RECOLOR_DESIGN.md:
//
//   * The replacement swaps a track's VALUES, never its timing. The `.m2`
//     still says when the colour changes.
//   * Alpha is untouched — the row's alpha byte is never read.
//   * The colour track must have exactly three keys, because the replacement
//     array is indexed by the track's own key indices. 2 369 of the 2 375
//     shipped carriers satisfy that; the six that do not are skipped rather
//     than read past, which is what retail does in a release build.
//
// A colour id of 0 means "leave the model alone" and is the overwhelming
// majority. A NON-zero id that names no row is not the same thing: the client
// fills all three slots with opaque green instead, and @ref Resolve reproduces
// that rather than silently doing nothing — a green creature is a missing
// database row, and hiding it would hide the reason.
// ============================================================================

#include "whiteout/flakes/model_types.h"
#include "whiteout/flakes/types.h"

#include <unordered_map>

namespace whiteout::flakes::io {
class IContentProvider;
class ProgressMonitor;
} // namespace whiteout::flakes::io

namespace whiteout::flakes::io::wow {

/// `ParticleColor.db2`, parsed. A value rather than a global for the same
/// reason CreatureSkinTable is: the rows belong to the install they came from.
class ParticleColorTable {
public:
    /// Read the table through @p provider. Idempotent — a second call after a
    /// successful load does nothing, and after a failed one tries again.
    /// @param progress Optional; see CreatureSkinTable::Load.
    bool Load(IContentProvider& provider, ProgressMonitor* progress = nullptr);

    bool Loaded() const noexcept {
        return loaded_;
    }

    void Clear();

    /// Fill @p out with what the client would apply for colour id @p id.
    ///
    /// Returns false for id 0 — nothing to replace, and the model keeps its own
    /// colours. Returns true for any non-zero id, filling @p out from the row
    /// when there is one and with the client's opaque green when there is not.
    bool Resolve(u32 id, renderer::M2ParticleColorOverride& out) const;

    /// Rows read, for status display. Zero until a successful Load.
    usize RowCount() const noexcept {
        return rows_.size();
    }

private:
    bool loaded_ = false;
    std::unordered_map<u32, renderer::M2ParticleColorOverride> rows_;
};

} // namespace whiteout::flakes::io::wow
