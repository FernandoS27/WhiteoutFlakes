#pragma once

// ============================================================================
// The conversion front door: what the active document can be written as, and
// the writes themselves.
//
// Save As writes a model back in its own format (MDX/MDL, `.m3`, an effect's
// bytes); everything else CONVERTS through WEM. Which of those a document
// offers is asked of the focus actor's SOURCE, once, rather than of the file
// extension: a `.wem` opened as Warcraft III is an MDX model in memory, and a
// document with no model is nothing.
// ============================================================================

#include "export/export_common.h"
#include "export/export_options.h"
#include "whiteout/flakes/types.h"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace whiteout::flakes::renderer {
class RenderService;
}

namespace whiteout::flakes {

class DocumentManager;
class PlaybackController;

struct ModelCapabilities {
    /// Save As MDX/MDL: a Warcraft III source, or an effect copied verbatim.
    bool saveAsMdx = false;
    /// Save As for a source that IS `.m3`.
    bool saveM3 = false;
    /// Any source WEM reads; the Export submenu's gate.
    bool exportWem = false;
    /// Through WEM, for a source that is not already the target's game — that
    /// one has Save As, without deriving a material set it already carries.
    bool exportMdx = false;
    bool exportM3 = false;
    /// Any carried profile (GLTF_DESIGN §2): exactly exportWem.
    bool exportGltf = false;
};

/// How one write went, for the log and for a dialog that stays open on failure.
struct ExportOutcome {
    bool ok = false;
    /// Why it failed, or why nothing was attempted.
    std::string error;
    /// Nothing was attempted (no model on screen): logged as a note, not a failure.
    bool refused = false;
    /// Logged first, one line each: a refusal, or what a save had to give up.
    std::vector<std::string> notes;
    /// What a lossy conversion cost, already described; printed on success too,
    /// because that is when it is worth reading.
    usize diagnosticCount = 0;
    std::string diagnostics;
    /// What was written, one line each, on success.
    std::vector<std::string> summary;
};

/// The one place a write's outcome is printed: notes, diagnostics, then the
/// failure or the summary. @p operation is "Export M3", "Save M3", ...
void LogOutcome(std::string_view operation, const ExportOutcome& outcome);

class ModelExportService {
public:
    ModelExportService(renderer::RenderService& service, DocumentManager& documents,
                       PlaybackController& playback);

    ModelCapabilities Capabilities() const;

    ExportOutcome ExportWem(const std::filesystem::path& outPath);
    ExportOutcome ExportMdx(const std::filesystem::path& outPath, const MdxExportOptions& options);
    ExportOutcome ExportM3(const std::filesystem::path& outPath, const M3ExportOptions& options);
    ExportOutcome ExportGltf(const std::filesystem::path& outPath, const GltfExportOptions& options);
    ExportOutcome SaveM3(const std::filesystem::path& outPath, const M3SaveOptions& options);
    /// Save As MDX/MDL (see mdx_save.h); @p hiveDialect picks Hiveworkshop MDL.
    /// @p outPath is UTF-8. Logs its own outcome.
    bool SaveAsMdx(const std::string& outPath, bool hiveDialect, bool exportTextures,
                   const std::string& textureFormat);
    /// Save As for an effect: its bytes, verbatim. Logs its own outcome.
    bool SaveEffect(const std::string& outPath);

private:
    /// @p source written to @p outPath, resolving through the active scene and
    /// named for the active document.
    ExportSubject SubjectFor(renderer::model::IModelSource& source,
                             const std::filesystem::path& outPath) const;

    renderer::RenderService& service_;
    DocumentManager& documents_;
    PlaybackController& playback_;
};

} // namespace whiteout::flakes
