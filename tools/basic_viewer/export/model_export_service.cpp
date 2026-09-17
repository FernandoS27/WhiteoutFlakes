#include "export/model_export_service.h"

#include "documents/document_manager.h"
#include "documents/model_formats.h"
#include "documents/playback_controller.h"
#include "export/mdx_save.h"
#include "export/gltf_export.h"
#include "io/file_content_provider.h"
#include "io/mdx_model_adapter.h"
#include "io/wem/wem_export.h"
#include "io/wem/wem_import.h"
#include "export/m3_export.h"
#include "export/mdx_export.h"
#include "renderer/model/model_instance.h"
#include "renderer/model/model_template.h"
#include "renderer/render_service.h"
#include "renderer/scene_manager.h"
#include "settings_ini.h"
#include "whiteout/flakes/util/path_utf8.h"
#include "whiteout/flakes/util/replaceable_paths.h"
#if WDX_ENABLE_M3
#include "io/m3/m3_model_adapter.h"
#include "export/m3_save.h"
#endif

#include <cstdarg>
#include <cstdio>

namespace whiteout::flakes {

namespace {

namespace model = renderer::model;
namespace wem = ::whiteout::models::wem;

// A cross-format write is lossy by construction; the list of what it cost is
// capped where a game model's hundreds of dropped keys would bury the rest.
constexpr usize kConversionDiagnosticLines = 400;

std::string Format(const char* format, ...) {
    va_list args;
    va_start(args, format);
    va_list count;
    va_copy(count, args);
    const int size = std::vsnprintf(nullptr, 0, format, count);
    va_end(count);
    std::string out(size > 0 ? static_cast<usize>(size) : 0, '\0');
    if (size > 0)
        std::vsnprintf(out.data(), out.size() + 1, format, args);
    va_end(args);
    return out;
}

ExportOutcome Refused(std::string reason) {
    ExportOutcome outcome;
    outcome.refused = true;
    outcome.error = reason;
    outcome.notes.push_back(std::move(reason));
    return outcome;
}

void Describe(ExportOutcome& outcome, const wem::Diagnostics& diagnostics, usize maxLines) {
    if (diagnostics.empty())
        return;
    outcome.diagnosticCount = diagnostics.size();
    outcome.diagnostics = io::DescribeWemDiagnostics(diagnostics, maxLines);
}

// The WEM export keeps the describer's own short default.
void Describe(ExportOutcome& outcome, const wem::Diagnostics& diagnostics) {
    if (diagnostics.empty())
        return;
    outcome.diagnosticCount = diagnostics.size();
    outcome.diagnostics = io::DescribeWemDiagnostics(diagnostics);
}

bool IsMdxSource(const model::Actor* actor) {
    return actor && dynamic_cast<const io::MdxModelAdapter*>(actor->animation.Source().get()) != nullptr;
}

#if WDX_ENABLE_M3
const io::M3ModelAdapter* M3SourceOf(const model::Actor* actor) {
    return actor && actor->animation.HasSource()
               ? dynamic_cast<const io::M3ModelAdapter*>(actor->animation.Source().get())
               : nullptr;
}
#endif

bool IsM3Source(const model::Actor* actor) {
#if WDX_ENABLE_M3
    return M3SourceOf(actor) != nullptr;
#else
    (void)actor;
    return false;
#endif
}

} // namespace

void LogOutcome(std::string_view operation, const ExportOutcome& outcome) {
    const std::string op(operation);
    for (const std::string& note : outcome.notes)
        std::fprintf(stderr, "[viewer] %s: %s\n", op.c_str(), note.c_str());
    if (outcome.refused)
        return;
    if (outcome.diagnosticCount != 0)
        std::fprintf(stderr, "[viewer] %s: %zu diagnostic(s)\n%s", op.c_str(), outcome.diagnosticCount,
                     outcome.diagnostics.c_str());
    if (!outcome.ok) {
        std::fprintf(stderr, "[viewer] %s FAILED: %s\n", op.c_str(), outcome.error.c_str());
        return;
    }
    for (const std::string& line : outcome.summary)
        std::printf("[viewer] %s\n", line.c_str());
}

ModelExportService::ModelExportService(renderer::RenderService& service, DocumentManager& documents,
                                       PlaybackController& playback)
    : service_(service), documents_(documents), playback_(playback) {}

ExportSubject ModelExportService::SubjectFor(renderer::model::IModelSource& source,
                                             const std::filesystem::path& outPath) const {
    return {&source, service_.Scene().ActiveContentProvider(), outPath,
            io::PathToUtf8(documents_.ActiveState().modelPath.stem())};
}

ModelCapabilities ModelExportService::Capabilities() const {
    ModelCapabilities caps;
    const std::filesystem::path& path = documents_.ActiveState().modelPath;
    const model::Actor* actor = playback_.FocusActor();

    if (actor && actor->animation.Source()) {
        const auto* source = dynamic_cast<const renderer::model::IModelSource*>(actor->animation.Source().get());
        caps.exportWem = source && io::CanExportModelToWem(*source);
    }
    caps.exportMdx = caps.exportWem && actor && !IsMdxSource(actor);
#if WDX_ENABLE_M3
    caps.exportM3 = caps.exportWem && actor && !IsM3Source(actor);
#else
    caps.exportM3 = caps.exportWem;
#endif
    caps.exportGltf = caps.exportWem;
    caps.saveM3 = IsM3Source(actor);

    // A PopcornFX effect is copied verbatim rather than written, so it answers
    // yes on its own terms. A `.m2` / `.m3` has no MDX to write whatever it holds.
    if (!path.empty() && !IsModelKind(path, ModelKind::ForeignModel)) {
        const bool templateIsMdx =
            actor && actor->sourceTemplate &&
            dynamic_cast<const io::MdxModelAdapter*>(actor->sourceTemplate->adapter.get()) != nullptr;
        caps.saveAsMdx = IsModelKind(path, ModelKind::Effect) || templateIsMdx || IsMdxSource(actor);
    }
    return caps;
}

ExportOutcome ModelExportService::ExportWem(const std::filesystem::path& outPath) {
    model::Actor* actor = playback_.FocusActor();
    if (!actor || !actor->animation.Source())
        return Refused("no model on screen");
    auto* source = dynamic_cast<renderer::model::IModelSource*>(actor->animation.Source().get());
    if (!source)
        return Refused("this actor has no model source");

    ExportOutcome outcome;
    io::WemExportOptions options;
    options.documentName = io::PathToUtf8(documents_.ActiveState().modelPath.stem());
    const io::WemExportResult exported =
        io::ExportModelToWem(*source, service_.Scene().ActiveContentProvider(), options);
    if (!exported.ok()) {
        outcome.error = exported.error;
        return outcome;
    }
    // A lossy conversion that succeeded and one that failed are different
    // states, and this is the one the user can act on — a particle emitter the
    // format does not carry, a look that did not resolve.
    Describe(outcome, exported.diagnostics);

    wem::Diagnostics writeReport;
    if (!io::WriteWemDocument(*exported.document, outPath, &writeReport, &outcome.error))
        return outcome;
    outcome.ok = true;
    outcome.summary.push_back(
        Format("Saved WEM (%s): %s", exported.formatId.c_str(), io::PathToUtf8(outPath).c_str()));
    return outcome;
}

// The other half of Save As: that one re-serialises a Warcraft III model in its
// own format; this converts a foreign one *into* Warcraft III, which is the
// whole reason the interchange format exists. See mdx_export.h.
ExportOutcome ModelExportService::ExportMdx(const std::filesystem::path& outPath, const MdxExportOptions& options) {
    model::Actor* actor = playback_.FocusActor();
    auto* source = actor ? dynamic_cast<renderer::model::IModelSource*>(actor->animation.Source().get()) : nullptr;
    if (!source)
        return Refused("no model on screen");

    MdxExportRequest request;
    request.subject = SubjectFor(*source, outPath);
    request.options = options;

    const MdxExportReport report = ExportModelAsMdx(request);
    ExportOutcome outcome;
    Describe(outcome, report.diagnostics, kConversionDiagnosticLines);
    if (!report.ok) {
        outcome.error = report.error;
        return outcome;
    }
    outcome.ok = true;
    outcome.summary.push_back(Format("Saved Warcraft III model (%s, %gx scale): %s", report.formatId.c_str(),
                                     static_cast<double>(report.scale), io::PathToUtf8(outPath).c_str()));
    if (options.textures)
        outcome.summary.push_back(Format("Textures: %d exported, %d skipped, %d failed, %d unused",
                                         report.textures.exported, report.textures.skipped, report.textures.failed,
                                         report.textures.unused));
    return outcome;
}

// ExportMdx's twin: a `.mdx`, a `.m2` or a Diablo III `.app` converted through
// WEM and written as `.m3`. See m3_export.h.
ExportOutcome ModelExportService::ExportM3(const std::filesystem::path& outPath, const M3ExportOptions& options) {
    model::Actor* actor = playback_.FocusActor();
    auto* source = actor ? dynamic_cast<renderer::model::IModelSource*>(actor->animation.Source().get()) : nullptr;
    if (!source)
        return Refused("no model on screen");

    M3ExportRequest request;
    request.subject = SubjectFor(*source, outPath);
    request.options = options;
    // The tileset the viewer resolves replaceables with is the one the export
    // resolves them with.
    request.tileset = GetCurrentTileset();
    if (options.war3ModTextures) {
        // The StarCraft II root Settings names, else the one the scan found.
        io::FileContentProvider& provider = service_.DefaultScene().GetContentProvider();
        request.starCraft2Install = provider.Game() == ProductId::Sc2
                                        ? provider.InstallPath()
                                        : LoadIoPathOverrides(ProductId::Sc2).installPath;
        if (request.starCraft2Install.empty())
            request.starCraft2Install = provider.GamePath(ProductId::Sc2);
    }

    const M3ExportReport report = ExportModelAsM3(request);
    ExportOutcome outcome;
    Describe(outcome, report.diagnostics, kConversionDiagnosticLines);
    if (!report.ok) {
        outcome.error = report.error;
        return outcome;
    }
    outcome.ok = true;
    outcome.summary.push_back(Format("Saved StarCraft II model (%s, %gx scale): %s", report.formatId.c_str(),
                                     static_cast<double>(report.scale), io::PathToUtf8(outPath).c_str()));
    if (report.particleRecords != 0 || report.ribbonRecords != 0 || report.cameraRecords != 0 ||
        report.hitTests != 0)
        outcome.summary.push_back(Format("Effects: %d PAR_ (%d model particles), %d RIB_, %d CAM_ aimed, "
                                         "%d hit tests, %d spawned models written",
                                         report.particleRecords, report.modelParticles, report.ribbonRecords,
                                         report.cameraRecords, report.hitTests, report.spawnedModels));
    if (options.textures)
        outcome.summary.push_back(Format("Textures: %d exported, %d skipped, %d failed, %d unused, %d in War3 (Mod)",
                                         report.textures.exported, report.textures.skipped, report.textures.failed,
                                         report.textures.unused, report.textures.inWar3Mod));
    return outcome;
}

ExportOutcome ModelExportService::ExportGltf(const std::filesystem::path& outPath, const GltfExportOptions& options) {
    model::Actor* actor = playback_.FocusActor();
    auto* source = actor ? dynamic_cast<renderer::model::IModelSource*>(actor->animation.Source().get()) : nullptr;
    if (!source)
        return Refused("no model on screen");

    GltfExportRequest request;
    request.subject = SubjectFor(*source, outPath);
    request.options = options;
    request.teamColor = actor->teamColor;
    // Read under the tier the scene draws. A render-mode change forwards its
    // implied tier to the shared provider and only the next pump re-imposes the
    // scene's own, so an export straight after a load read Definitive art
    // through the classic overlay and embedded none of it.
    if (request.subject.provider != nullptr)
        request.subject.provider->SetArtTier(service_.EffectiveArtTier());

    const GltfExportReport report = ExportModelAsGltf(request);
    ExportOutcome outcome;
    Describe(outcome, report.diagnostics, kConversionDiagnosticLines);
    if (!report.ok) {
        outcome.error = report.error;
        return outcome;
    }
    outcome.ok = true;
    outcome.summary.push_back(Format("Saved glTF (%s, %s profile): %s", report.formatId.c_str(),
                                     wem::Profile(report.profile).displayName, io::PathToUtf8(outPath).c_str()));
    if (options.textures)
        outcome.summary.push_back(Format("Textures: %d %s, %d failed", report.textures.exported,
                                         options.binary ? "embedded" : "exported", report.textures.failed));
    return outcome;
}

bool ModelExportService::SaveAsMdx(const std::string& outPath, bool hiveDialect, bool exportTextures,
                                   const std::string& textureFormat) {
    const auto dialect = hiveDialect ? whiteout::mdx::MdlFormat::Hiveworkshop : whiteout::mdx::MdlFormat::WarcraftIII;
    return SaveModelAsMdx(service_, playback_.FocusActor(), documents_.ActiveState().modelPath, outPath, dialect,
                          exportTextures, textureFormat);
}

bool ModelExportService::SaveEffect(const std::string& outPath) {
    return SaveEffectCopy(service_, documents_.ActiveState().modelPath, outPath);
}

// ExportM3's mirror: that one derives a StarCraft II material set for a model
// that never had one; this writes back a model that already IS `.m3`, straight
// through `m3::Writer` and not through WEM. See m3_save.h.
ExportOutcome ModelExportService::SaveM3(const std::filesystem::path& outPath, const M3SaveOptions& options) {
#if WDX_ENABLE_M3
    const io::M3ModelAdapter* source = M3SourceOf(playback_.FocusActor());
    if (!source)
        return Refused("no StarCraft II model on screen");

    M3SaveRequest request;
    request.source = source;
    request.provider = service_.Scene().ActiveContentProvider();
    request.outPath = outPath;
    request.options = options;

    const M3SaveReport report = SaveModelAsM3(request);
    ExportOutcome outcome;
    // What the merge and the retarget gave up, one line each.
    for (const wem::Diagnostic& lossy : report.diagnostics.all())
        outcome.notes.push_back(lossy.message);
    if (!report.ok) {
        outcome.error = report.error;
        return outcome;
    }
    outcome.ok = true;
    outcome.summary.push_back(Format("Saved M3 (MODL v%d%s%s): %s", report.version,
                                     report.retargeted ? ", retargeted for StarCraft II" : "",
                                     report.mergedFiles ? ", animations merged" : "",
                                     io::PathToUtf8(outPath).c_str()));
    if (report.mergedFiles)
        outcome.summary.push_back(Format("Merged %zu animation file(s), %zu sequence(s)", report.mergedFiles,
                                         report.mergedSequences));
    if (options.textures)
        outcome.summary.push_back(Format("Textures: %d exported, %d skipped, %d failed", report.textures.exported,
                                         report.textures.skipped, report.textures.failed));
    return outcome;
#else
    (void)outPath;
    (void)options;
    ExportOutcome nothing;
    nothing.refused = true;
    return nothing;
#endif
}

} // namespace whiteout::flakes
