#pragma once

// ============================================================================
// Opening a `.wem` — parse, pick a profile, and build the NATIVE model the
// renderer's own adapter draws.
//
// The one architectural rule this file exists to obey (WEM_DESIGN.md N1):
// WEM is not a renderer IR. Nothing here implements IModelSource over a
// wem::Document, and nothing in a per-frame path ever sees one. A document is
// converted to `mdx::Model` / `m2::Model` / `m3::Model` in memory and handed to
// MdxModelAdapter / M2ModelAdapter / M3ModelAdapter — the same constructors the
// 3ds Max plugin drives the renderer through — so the entire per-profile render
// stack is reached without one line of it being rewritten against a WEM type.
//
// The profile is an INPUT, not something sniffed out of the file. A document
// can carry several material sets over one geometry (§6.3), so "open this file"
// has as many answers as it has profiles; io/wem/wem_profiles.h is what a host
// draws the choice from. A caller with no opinion passes ProfileId::Count and
// gets DefaultWemProfile's answer.
//
// See WEM_INTEGRATION_DESIGN.md §1 and §3.
// ============================================================================

#include "io/wem/wem_profiles.h"
#include "whiteout/flakes/content_ref.h"
#include "whiteout/flakes/model_source.h"
#include "whiteout/flakes/types.h"

#include <whiteout/models/wem/diagnostics.h>
#include <whiteout/models/wem/document.h>

#include <filesystem>
#include <memory>
#include <span>
#include <string>

namespace whiteout::flakes::io {

class IContentProvider;
class D3SnoCache;

/// @brief True when @p data is a WEM file this build can read.
///
/// Magic + version only, which is what `wem::IsWemFile` promises. Cheap enough
/// to sit in the loader's format cascade beside LooksLikeM2 / LooksLikeM3.
bool LooksLikeWem(std::span<const ::whiteout::u8> data);

/// @brief A parsed `.wem`, kept so a host can ask what it offers before
///        committing to a profile.
///
/// The document is held by value because `DeriveProfile` mutates one and a
/// caller that opens the same file as two profiles must not have the first
/// derive leak into the second. @ref BuildWemSource copies before it derives.
struct WemDocument {
    wem::Document document;
    /// What the parse said. A preserved chunk this build did not understand is
    /// neither a success nor a failure (parser.h), so the report is kept rather
    /// than reduced to a bool.
    wem::Diagnostics diagnostics;
    /// How the file was named, for messages. Never parsed for meaning.
    std::string name;
};

/// @brief Parse @p data. Null when it is not a WEM file or the parse failed;
///        @p name is used only in diagnostics.
std::shared_ptr<WemDocument> ParseWemDocument(std::span<const ::whiteout::u8> data,
                                              std::string name);

/// @brief The same, reading the file itself. Convenience for hosts that have a
///        path rather than bytes.
std::shared_ptr<WemDocument> ParseWemFile(const std::filesystem::path& path);

/// @brief What one open produced.
struct WemSourceResult {
    /// Null on failure; @ref error then says why in one line.
    std::shared_ptr<renderer::model::IModelSource> source;
    /// The profile actually used — resolved from `ProfileId::Count`, and never
    /// silently different from a profile the caller named.
    wem::ProfileId profile = wem::ProfileId::Count;
    /// Which scene product the result wants. The caller sets it BEFORE the
    /// spawn: the product selects the storage every texture reference resolves
    /// against, and a WoW model in a Warcraft III scene loses every texture.
    ProductId product = ProductId::Neutral;
    /// Warcraft III's HD generation, so the host applies RenderMode::HD.
    bool hd = false;
    /// Whether the profile's set had to be derived (§6.6) — always lossy, and
    /// the diagnostics say by how much.
    bool derived = false;
    /// Which install this document's assets live in, from the profile it was
    /// AUTHORED in rather than the one it was opened as.
    ///
    /// Equal to @ref product until a document is opened as a profile it does
    /// not carry, and then it is the interesting one: a Diablo III appearance
    /// opened as Warcraft III draws under Warcraft III's render profile and
    /// still names Diablo III textures by SNO. Resolving those against a
    /// Warcraft III install finds none of them.
    ProductId assetProduct = ProductId::Neutral;
    /// Game units → renderer units for the geometry this built, from the
    /// profile it was AUTHORED in rather than the one it was opened as.
    ///
    /// The two are the same number until a document is opened as a profile it
    /// does not carry. WEM does not normalise scale — geometry stays in the
    /// units it was authored in (`Document::unitScale`) — and a converter only
    /// restates the material set, so a Diablo III appearance opened as
    /// Warcraft III is still a 8.5-unit Barbarian. Stamping it with Warcraft
    /// III's 1.0 draws it at a seventeenth of the size, which reads as a smudge
    /// on the grid rather than as a scale bug.
    f32 worldScale = 1.0f;
    /// Conversion + derive diagnostics, in that order. Not empty on success:
    /// every `toX` reports the clips it did not write (see §2 of the design).
    wem::Diagnostics diagnostics;
    std::string error;

    bool ok() const {
        return source != nullptr;
    }
};

/// @brief Build the native model source for @p profile out of @p document.
///
/// @param profile `ProfileId::Count` means "decide" — @ref DefaultWemProfile.
///        A profile the document does not carry is DERIVED from
///        @ref WemDeriveSource's answer first, which is lossy and says so.
/// @param basePath the directory relative texture paths resolve against, which
///        for a Warcraft III model is the folder the `.wem` sits in.
/// @param provider the scene's content provider, forwarded to the MDX adapter
///        so an archive-resident texture still resolves. May be null.
/// @param d3Cache where a Diablo III document's converted appearance and its
///        animations are registered. `D3ModelAdapter` reaches every asset
///        through this cache and through nothing else, so opening a Diablo III
///        `.wem` needs one; without it that profile is refused rather than
///        opened as a statue.
WemSourceResult BuildWemSource(const WemDocument& document, wem::ProfileId profile,
                               const std::filesystem::path& basePath = {},
                               IContentProvider* provider = nullptr, D3SnoCache* d3Cache = nullptr);

/// @brief One line per diagnostic severity, for a log or a dialog.
///
/// Empty when @p diagnostics is. Written here rather than at each call site so
/// the loader, the viewer and the tests describe a conversion the same way.
std::string DescribeWemDiagnostics(const wem::Diagnostics& diagnostics, usize maxLines = 8);

} // namespace whiteout::flakes::io
