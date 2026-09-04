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
