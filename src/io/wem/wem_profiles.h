#pragma once

// ============================================================================
// The WEM profile axis, as this renderer sees it.
//
// A `wem::ProfileId` names a game and its shading generation; a `ProductId`
// names which game's data a scene holds. They are the same axis seen from two
// sides — WEM splits Warcraft III into classic/Reforged and StarCraft II into
// SC2/Heroes, and the scene does not — so the mapping is many-to-one and this
// table is where it is stated once.
//
// Deliberately free of every renderer type. The viewer's profile dialog has to
// answer "what does this file offer, and what can this build open it as" before
// anything is spawned, and a dialog that had to build a model source to find
// out would parse the document twice and pull the whole render stack into a
// header the UI includes. So: WhiteoutLib in, plain data out.
//
// See WEM_INTEGRATION_DESIGN.md §3.
// ============================================================================

#include "whiteout/flakes/enums.h"
#include "whiteout/flakes/types.h"

#include <whiteout/models/wem/document.h>
#include <whiteout/models/wem/profile.h>

#include <filesystem>
#include <string>
#include <vector>

namespace whiteout::flakes::io {

namespace wem = ::whiteout::models::wem;

// ---------------------------------------------------------------------------
// Static facts about a profile
// ---------------------------------------------------------------------------

/// @brief Which scene product a WEM profile draws in.
///
/// Total, and `Wc3` is the fallback: a profile with no product would spawn into
/// whichever scene happened to be open, and the product is what selects the
/// storage every texture reference resolves against.
ProductId ProductForWemProfile(wem::ProfileId profile);

/// @brief Whether this build can turn a document into a native model for
///        @p profile — the `WDX_ENABLE_*` answer, not a statement about the
///        document.
///
/// False for one profile whatever the build: `Generic` names no game, so it has
/// no `formatId` and no converter serves it. It is a derive *source*, never a
/// target (§3).
///
/// `Diablo3` used to be the second, on the grounds that writing SNO is a
/// separate project (WEM_DESIGN.md §18). It still is — but a viewer never
/// needed a file: `D3Converter::toAppearance` builds the native `Appearances`
/// in memory, which is what the other three profiles do with their format's
/// struct. So this is a build flag now and nothing more.
bool WemProfileOpenable(wem::ProfileId profile);

/// @brief Why not, for a profile @ref WemProfileOpenable refuses. Empty when it
///        does not refuse. English, and short enough to sit on a dialog row.
const char* WemProfileUnsupportedReason(wem::ProfileId profile);

/// @brief `wem::ProfileId` ↔ its registry name, for settings and the CLI.
/// Returns `ProfileId::Count` for a name no profile carries.
wem::ProfileId WemProfileFromName(const std::string& name);

/// @brief Whether this WEM profile is Warcraft III's HD generation, which the
///        host applies as `RenderMode::HD` before the spawn.
bool WemProfileIsHd(wem::ProfileId profile);

/// @brief The `.mdx` version a Warcraft III profile is written at.
///
/// The two profiles are one file format at two versions — 800 is classic's and
/// 1000 is what Reforged writes — and `mdx_core::ExportMaterial` is what
/// actually decides whether a layer is HD, so this only has to agree with the
/// material rather than drive it. Zero for a profile that is not Warcraft III.
u32 MdxVersionForWemProfile(wem::ProfileId profile);

/// @brief Whether @p path is named like a WEM file.
///
/// The *name*, not the content: a host that has not read the bytes yet — the
/// CLI deciding which spawn entry point to call — has nothing else to go on.
/// Everything that has the bytes uses `LooksLikeWem` (io/wem/wem_import.h),
/// which is the honest test.
bool LooksLikeWemPath(const std::filesystem::path& path);

// ---------------------------------------------------------------------------
// What one document offers
// ---------------------------------------------------------------------------

/// @brief One row of the profile picker.
struct WemProfileOption {
    wem::ProfileId profile = wem::ProfileId::Generic;
    /// The registry name ("wc3_classic", "wow", …) and the human one.
    const char* name = "";
    const char* displayName = "";

    /// The document carries a `ProfileMaterialSet` for it.
    bool carried = false;
    /// At least one section of at least one model draws in it.
    bool drawn = false;
    /// This build can open a document as it.
    bool supported = false;
    /// Not carried: opening runs `DeriveProfile` from @ref deriveFrom first.
    bool derived = false;
    /// Which profile a derive would read. Only meaningful when @ref derived.
    wem::ProfileId deriveFrom = wem::ProfileId::Generic;
};

/// @brief The picker's rows for @p document, one per `wem::ProfileId` that is
///        worth offering, most-preferred first.
///
/// Order: carried and supported, then supported derives, then everything this
/// build cannot open. Nothing is filtered out — a file that says "Diablo III"
/// and a dialog with no Diablo III row is a bug report waiting to happen — so a
/// caller draws the unsupported rows disabled with
/// @ref WemProfileUnsupportedReason beside them.
std::vector<WemProfileOption> WemProfileOptions(const wem::Document& document);

/// @brief What @ref WemProfileOptions would select on its own.
///
/// `Document::defaultProfile` when this build can open it, the first carried
/// and supported profile otherwise, and `ProfileId::Count` when there is none —
/// which is the state a headless load reports rather than guessing.
wem::ProfileId DefaultWemProfile(const wem::Document& document);

/// @brief Which profile a derive to @p target should read from.
///
/// The document's default when it carries it, else the first carried profile,
/// else `ProfileId::Count`. Kept beside the option list because the loader has
/// to make the same choice the dialog showed.
wem::ProfileId WemDeriveSource(const wem::Document& document, wem::ProfileId target);

} // namespace whiteout::flakes::io
