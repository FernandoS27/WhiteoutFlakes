#pragma once

// ============================================================================
// What every model conversion shares: the model it writes and where, the part
// of its report every caller reads, the texture counters, and the hop through
// WEM each foreign-format export starts with.
// ============================================================================

#include "whiteout/flakes/model_source.h"
#include "whiteout/flakes/types.h"

#include <whiteout/models/wem/diagnostics.h>
#include <whiteout/models/wem/document.h>

#include <filesystem>
#include <functional>
#include <optional>
#include <string>

namespace whiteout::flakes {

namespace io {
class IContentProvider;
struct WemExportOptions;
}

/// The model a conversion writes, and where.
struct ExportSubject {
    /// The model on screen. Must be one `io::CanExportModelToWem` accepts.
    renderer::model::IModelSource* source = nullptr;
    /// Where every texture reference resolves. Null exports the model alone.
    io::IContentProvider* provider = nullptr;

    std::filesystem::path outPath;
    /// Names the model inside the file, and the fallback stem for a texture
    /// the source addressed by id and never named.
    std::string modelName;
};

/// What an export did with the textures of the model it wrote.
struct TextureExportCounters {
    int exported = 0;
    int skipped = 0;   ///< Copies already present at the target (a bake is rewritten).
    int failed = 0;    ///< Unresolvable, undecodable or unwritable.
    int unused = 0;    ///< Nothing the written model reads names them; not written.
    int inWar3Mod = 0; ///< Named at War3 (Mod)'s own copy instead of written.

    TextureExportCounters& operator+=(const TextureExportCounters& other);
};

/// The part of a report every conversion fills, in the shape a log line and a
/// dialog both want.
struct ConversionReport {
    bool ok = false;
    std::string error;

    /// The converter the model came through — "mdx", "m2", "m3", "d3".
    std::string formatId;
    /// The factor the geometry was restated at: 100 from World of Warcraft or
    /// StarCraft II into Warcraft III, 1/100 the other way, 1 within a game.
    f32 scale = 1.0f;

    /// Conversion + derive + rescale diagnostics. Never empty after a
    /// cross-format write: there is always something it could not carry.
    ::whiteout::models::wem::Diagnostics diagnostics;
};

/// Create @p path's folder, run @p write with the path in UTF-8, and check the
/// file landed. A writer's failed open is not an exception — an `ofstream`
/// that could not open writes nothing and says nothing — so the check is the
/// only thing that notices. Empty on success, otherwise why not.
std::string WriteModelFile(const std::filesystem::path& path,
                           const std::function<void(const std::string& utf8Path)>& write);

/// @p subject's model as a WEM document. Appends what the export cost to
/// @p report and, on failure, fails it and returns nothing.
std::optional<::whiteout::models::wem::Document> ConvertThroughWem(
    const ExportSubject& subject, const io::WemExportOptions& options, ConversionReport& report);

} // namespace whiteout::flakes
