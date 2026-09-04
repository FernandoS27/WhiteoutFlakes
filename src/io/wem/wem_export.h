#pragma once

// ============================================================================
// Writing a `.wem` from whatever the renderer has on screen.
//
// The export direction is the one WhiteoutLib's converters were already pointed
// at — `fromMdx` / `fromM2` / `fromM3` / `fromAppearance` all take the PARSED
// native model, and every adapter keeps its own (`SourceModel()`,
// `SourceAppearance()`) precisely so a host can re-serialise without re-reading
// the file. So this is a dispatch on the adapter's concrete type and nothing
// else; there is no format-neutral path through ModelData, because that would
// throw away exactly the native blocks (§7.3) the file exists to carry.
//
// A source this does not recognise — a live Max-plugin adapter, a host's own
// IModelSource — is refused by name rather than written as an empty document.
//
// See WEM_INTEGRATION_DESIGN.md §5.
// ============================================================================

#include "whiteout/flakes/model_source.h"
#include "whiteout/flakes/types.h"

#include <whiteout/models/mdx/structures.h>
#include <whiteout/models/wem/diagnostics.h>
#include <whiteout/models/wem/document.h>

#include <filesystem>
#include <optional>
#include <string>

namespace whiteout::flakes::io {

class IContentProvider;

namespace wem = ::whiteout::models::wem;

struct WemExportOptions {
    /// `Document::name`. Empty keeps whatever the converter derived from the
    /// model itself, which for MDX is its `modelName`.
    std::string documentName;

    /// Diablo III only: which look's materials are written. Empty means the
    /// look the actor is currently wearing, which is what the user is looking
    /// at and therefore what "export this" means.
    std::string materialLook;

    /// Diablo III only: follow `snoAnimSet` and every `.ani` it names. The
    /// clips are the expensive half of a D3 import (one `.ans` plus an `.ani`
    /// per tag), and a caller writing a thumbnail-grid's worth of files may not
    /// want them.
    bool importAnimation = true;

    /// StarCraft II / Heroes only: rewrite a Heroes-of-the-Storm model as the
    /// StarCraft II model it would have been before converting it.
    ///
    /// MD34 is one container that two engines read differently, and Heroes went
    /// one way with it: `MADD`, the data-driven material, plus the `MODL` v30
    /// that carries it. Measured over 18,409 shipped Heroes `.m3`, **686 (3.7%)
    /// are Heroes-only** and every one of them is v30 — 685 for MADD and one
    /// for a `REF_` above v2. `m3::toStarCraft2` reverses every MADD record
    /// into a `StandardMaterial`, repoints the material map and lowers the
    /// version; it manages **542 of the 686**, and names the material it cannot
    /// reverse on the other 144.
    ///
    /// Off by default, and deliberately: a `.wem` of a Heroes model should stay
    /// a Heroes document, because the native block is the whole reason the
    /// format exists and the MADD blob is what it would be carrying. An export
    /// to a *third* format has no such stake — Warcraft III cannot express a
    /// shader graph either — so `ExportModelAsMdx` asks for it and the plain
    /// `.wem` save does not.
    ///
    /// The document still declares the profile the *source* was: a Heroes model
    /// retargeted for the crossing is still Heroes content, and its textures
    /// belong under `Heroes\` rather than in StarCraft II's folder.
    bool retargetHeroesToStarCraft2 = false;
};

struct WemExportResult {
    /// Absent on failure; @ref error then says why.
    std::optional<wem::Document> document;
    /// The converter that ran — "mdx", "m2", "m3", "d3".
    std::string formatId;
    /// Conversion diagnostics. Non-empty is normal: a lossy conversion that
    /// succeeded and one that failed are different states and both have
    /// something to say (converter_base.h).
    wem::Diagnostics diagnostics;
    std::string error;

    bool ok() const {
        return document.has_value();
    }
};

/// @brief Convert @p source — an adapter this build recognises — to a document.
///
/// @param provider needed only for Diablo III, whose materials live on assets
///        the appearance merely names; without one the geometry, the nodes and
///        the slot join are identical and the render state is missing.
WemExportResult ExportModelToWem(renderer::model::IModelSource& source,
                                 IContentProvider* provider = nullptr,
                                 const WemExportOptions& options = {});

// ============================================================================
// Warcraft III
// ============================================================================

struct MdxExportOptions {
    /// Which Warcraft III generation is written. The two are one file format at
    /// two versions and one document at two material sets, so this picks both.
    wem::ProfileId profile = wem::ProfileId::Wc3Reforged;

    /// @brief Restate the geometry in Warcraft III's units.
    ///
    /// On by default, and the difference between a model and a smudge. WEM does
    /// not normalise scale — geometry stays in the units it was authored in —
    /// and a World of Warcraft creature is two units tall where a Warcraft III
    /// footman is a hundred. An in-memory open compensates by stamping the
    /// actor's `worldScale`; a file written to disk has no host to stamp, so the
    /// ratio has to be baked in. The factor is
    /// `wem::RescaleFactorBetween(authored, profile)`.
    bool rescale = true;

    /// @brief Write only the base level of detail.
    ///
    /// A World of Warcraft model carries one mesh per skin profile and a Diablo
    /// III appearance ships two geoset arrays; Warcraft III has no LOD gate
    /// below `.mdx` v1000 and draws every geoset in the file, so a classic
    /// export of a cow came out with its LOD 1 and LOD 2 stacked on top of it
    /// as a white sheet. Nothing downstream wants the ladder — a map draws its
    /// models at one distance — so it is dropped rather than carried.
    bool baseLodOnly = true;
};

struct MdxExportResult {
    /// Absent on failure; @ref error then says why in one line.
    std::optional<::whiteout::mdx::Model> model;
    /// The factor @ref MdxExportOptions::rescale applied, for the log line.
    f32 scale = 1.0f;
    /// Whether the Warcraft III material set had to be derived (§6.6), which is
    /// always lossy — every `.m2`, `.m3` and `.app` export is.
    bool derived = false;
    /// Meshes above the base level of detail that were not written.
    u32 lodMeshesDropped = 0;
    wem::Diagnostics diagnostics;
    std::string error;

    bool ok() const {
        return model.has_value();
    }
};

/// @brief Convert @p document to the Warcraft III model @ref MdxExportOptions
///        names, deriving, restating and rescaling on the way.
///
/// The write-to-disk twin of `BuildWemSource`'s MDX arm: the same staging, and
/// then the model itself rather than an adapter over it. Kept apart from that
/// one because the two differ in exactly the way a file differs from a view —
/// a file has no host to stamp a `worldScale` on, so it is rescaled, and its
/// textures have to be named rather than keyed by id.
MdxExportResult ConvertWemToMdx(const wem::Document& document,
                                const MdxExportOptions& options = {});

/// @brief Write @p document to @p path. Returns false and fills @p error on
///        failure; the writer's own diagnostics land in @p diagnostics.
bool WriteWemDocument(const wem::Document& document, const std::filesystem::path& path,
                      wem::Diagnostics* diagnostics = nullptr, std::string* error = nullptr);

/// @brief Whether @p source is a model this build can export at all.
///
/// What a host greys the menu item out on. Cheap — four dynamic_casts — and it
/// asks the same question @ref ExportModelToWem answers, so the menu and the
/// action cannot disagree.
bool CanExportModelToWem(const renderer::model::IModelSource& source);

} // namespace whiteout::flakes::io
