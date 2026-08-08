#include "viewer_ui.h"

#include "imgui_viewcube.h"
#include "io/mdx_model_adapter.h"
#include "renderer/assets/replaceable_texture_manager.h"
#include "renderer/camera.h"
#include "renderer/debug/debug_renderer.h"
#include "renderer/dnc/dnc_catalog.h"
#include "renderer/dnc/dnc_service.h"
#include "renderer/model/model_instance.h"
#include "renderer/model/model_template.h"
#include "renderer/particle/splat_service.h"
#include "renderer/render_service.h"
#include "renderer/scene_manager.h"
#include "renderer/shadow/shadow_service.h"
#include "localization.h"
#include "log_console.h"
#include "settings_ini.h"
#include "viewer_app.h"

#include "renderer/model/model_source_utils.h" // DispatchTextureParser (decode)
#include <whiteout/models/mdx/writer.h>
// Texture encoders for the Save As "export textures" option (every WhiteoutLib
// image writer except GIF).
#include <whiteout/textures/bmp/writer.h>
#include <whiteout/textures/blp/writer.h>
#include <whiteout/textures/dds/writer.h>
#include <whiteout/textures/jpeg/writer.h>
#include <whiteout/textures/png/writer.h>
#include <whiteout/textures/texture.h>
#include <whiteout/textures/tga/writer.h>
#include <whiteout/textures/tiff/writer.h>
#include "whiteout/flakes/content_provider.h"
#include "whiteout/flakes/display.h"
#include "whiteout/flakes/enums.h"
#include "whiteout/flakes/sound_emitter.h"
#include "whiteout/flakes/util/path_utf8.h"
#include "whiteout/flakes/util/replaceable_paths.h"

#include <imgui.h>
#include <nfd.hpp>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

namespace whiteout::flakes {

namespace {

// Persists current host state to WhiteoutFlakes.ini. Called after every UI
// change that should survive a restart — same call shape as the old
// HandleSettingsMessage paths used.
void SaveIni(const ViewerApp& app) {
    SaveSettingsIni(app.Service(), app.LoopNonLoopingPolicy(), app.ForceHd(),
                    i18n::languageCode(i18n::Localizer::instance().current()));
}

constexpr std::array<const char*, 10> kDebugVisLabels = {
    "Off",
    "Albedo",
    "World Normal",
    "LOD Heatmap",
    "Light Count",
    "Shading Only (white albedo)",
    "Shading Only (grey albedo)",
    "Specular Only (black albedo)",
    "No ORM",
    "AO Only",
};

constexpr std::array<const char*, 5> kLodLabels = {
    "Auto (screen size)", "Force LOD 0 (base)",   "Force LOD 1",
    "Force LOD 2",        "Force LOD 3 (lowest)",
};

constexpr std::array<const char*, 4> kIblLabels = {"Portrait", "Day/Night", "Dungeon", "Sunset"};
constexpr std::array<const char*, 3> kLightingLabels = {"InGame", "Glue", "Dynamic"};
constexpr std::array<const char*, 4> kShadowLabels = {"Off", "1 cascade", "2 cascades",
                                                      "3 cascades"};
constexpr std::array<const char*, 5> kBackendLabels = {"D3D11", "D3D12", "Vulkan", "WebGPU",
                                                       "Metal"};

// Parallel i18n key arrays for the visible label arrays above. Backend names
// are product/tech names and stay in English, so they get no key array.
constexpr std::array<const char*, 10> kDebugVisKeys = {
    "debugvis.off",           "debugvis.albedo",       "debugvis.world_normal",
    "debugvis.lod_heatmap",   "debugvis.light_count",  "debugvis.shading_white",
    "debugvis.shading_grey",  "debugvis.specular_only", "debugvis.no_orm",
    "debugvis.ao_only",
};
constexpr std::array<const char*, 5> kLodKeys = {
    "lod.auto", "lod.0", "lod.1", "lod.2", "lod.3",
};
constexpr std::array<const char*, 4> kIblKeys = {"ibl.portrait", "ibl.daynight", "ibl.dungeon",
                                                 "ibl.sunset"};
constexpr std::array<const char*, 3> kLightingKeys = {"lighting.ingame", "lighting.glue",
                                                      "lighting.dynamic"};
constexpr std::array<const char*, 4> kShadowKeys = {"shadow.off", "shadow.1", "shadow.2",
                                                    "shadow.3"};

i32 BackendToIdx(gfx::GfxApi b) {
    switch (b) {
    case gfx::GfxApi::D3D11:
        return 0;
    case gfx::GfxApi::D3D12:
        return 1;
    case gfx::GfxApi::Vulkan:
        return 2;
    case gfx::GfxApi::WebGPU:
        return 3;
    case gfx::GfxApi::Metal:
        return 4;
    }
    return 1;
}
gfx::GfxApi IdxToBackend(i32 idx) {
    switch (idx) {
    case 0:
        return gfx::GfxApi::D3D11;
    case 2:
        return gfx::GfxApi::Vulkan;
    case 3:
        return gfx::GfxApi::WebGPU;
    case 4:
        return gfx::GfxApi::Metal;
    default:
        return gfx::GfxApi::D3D12;
    }
}

} // namespace

ViewerUI::ViewerUI(ViewerApp& app) : app_(app) {
    // NFD's init / quit can be reference-counted; doing it once at first UI
    // construction matches its single-process expectations.
    NFD::Init();
}

void ViewerUI::BuildFrame() {
    BuildMenuBar();
    BuildToolbar();
    BuildTabBar();
    if (showViewCube_)
        BuildViewCubeWidget();
    if (settingsOpen_)
        BuildSettingsWindow();
    BuildSaveOptionsPopup();
    BuildExportPopup();
    app_.BuildStorageExplorerWindow();
    tools::LogConsole::Instance().DrawUi(&showLogConsole_);
}

void ViewerUI::BuildViewCubeWidget() {
    // The view-cube is a pure host-side ImGui widget — the renderer has no
    // notion of it. See tools/common/imgui_viewcube.h. Offset it below our
    // toolbar (+ tab bar, when documents are open) so it isn't tucked under the
    // top strips — both are menuH+8 tall and sit stacked under the menu bar.
    const f32 stripH = ImGui::GetFrameHeight() + 8.0f;
    const f32 topOffset = stripH + (app_.DocumentCount() > 0 ? stripH : 0.0f);
    tools::DrawViewCube(app_.Service().Scene().Camera(), topOffset);
}

void ViewerUI::OpenFileDialog() {
    // Use the UTF-8 NFD entry points so the filter strings stay as plain
    // `char` literals on every platform — the native variant takes wchar_t
    // on Windows, which would break these inline string constants.
    NFD::UniquePathU8 outPath;
    nfdu8filteritem_t filter[3] = {{"All supported", "mdx,mdl,pkb,pkfx"},
                                   {"Warcraft III Model", "mdx,mdl"},
                                   {"PKB Effect", "pkb,pkfx"}};
    if (NFD::OpenDialog(outPath, filter, 3) == NFD_OKAY) {
        std::filesystem::path p = io::FsPathFromUtf8(outPath.get());
        app_.LoadModel(p); // dispatches .pkb / .pkfx to the effect loader
    }
}

namespace {

// Image formats the Save As "export textures" option can convert to — every
// WhiteoutLib image writer except GIF. `ext` includes the dot (it's the output
// extension and what the model's texture path is rewritten to); "" means keep
// the source format. `label` is the format name (not localized); the empty
// entry uses the localized "keep original" string at draw time.
struct TexExportFormat {
    const char* ext;
    const char* label;
};
constexpr TexExportFormat kExportFormats[] = {
    {"", ""},        {".blp", "BLP"}, {".png", "PNG"},  {".tga", "TGA"},
    {".dds", "DDS"}, {".bmp", "BMP"}, {".jpg", "JPEG"}, {".tif", "TIFF"},
};

// Re-encode a decoded texture into `ext`. Converts to RGBA8 first — accepted by
// every writer and the common denominator across formats (BCn/paletted sources
// are decoded). Returns nullopt on an unknown ext or a writer failure.
std::optional<std::vector<u8>> EncodeTextureAs(const whiteout::textures::Texture& src,
                                               const std::string& ext) {
    namespace tx = whiteout::textures;
    tx::Texture t = src;
    t.format(tx::PixelFormat::RGBA8);
    auto run = [&](tx::Writer& w) -> std::optional<std::vector<u8>> {
        try {
            std::vector<u8> bytes = w.write(t);
            if (bytes.empty())
                return std::nullopt;
            return bytes;
        } catch (const std::exception&) {
            return std::nullopt;
        }
    };
    if (ext == ".png") { tx::png::Writer w; return run(w); }
    if (ext == ".tga") { tx::tga::Writer w; return run(w); }
    if (ext == ".blp") { tx::blp::Writer w; return run(w); }
    if (ext == ".dds") { tx::dds::Writer w; return run(w); }
    if (ext == ".bmp") { tx::bmp::Writer w; return run(w); }
    if (ext == ".jpg" || ext == ".jpeg") { tx::jpeg::Writer w; return run(w); }
    if (ext == ".tif" || ext == ".tiff") { tx::tiff::Writer w; return run(w); }
    return std::nullopt;
}

struct ExportStats {
    int exported = 0;
    int skipped = 0;
    int failed = 0;
};

// Writes the model's file-backed textures into `targetDir`, preserving each
// texture's relative path. A non-empty `formatExt` converts each texture to that
// format and rewrites the model's texture path to match (so the saved model
// references the exported files); "" keeps the referenced format (verbatim copy
// when the source bytes already match, else re-encoded to it — e.g. Reforged
// serves .dds for a .blp path). Textures already present at the target are left
// untouched. `model` is mutated (path rewrites); the caller writes it after.
ExportStats ExportModelTextures(ViewerApp& app, whiteout::mdx::Model& model,
                                const std::filesystem::path& targetDir,
                                const std::string& formatExt) {
    namespace fs = std::filesystem;
    ExportStats st;
    io::IContentProvider* provider = app.Service().Scene().ActiveContentProvider();

    auto lower = [](std::string s) {
        for (auto& c : s)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    };

    for (auto& tex : model.textures) {
        // replaceableId != 0 → runtime team-color/glow/tileset, no source file.
        if (tex.replaceableId != 0 || tex.fileName.empty())
            continue;

        const std::string origName = tex.fileName; // capture before any rewrite

        std::string relStr = origName;
        std::replace(relStr.begin(), relStr.end(), '\\', '/');
        const fs::path relPath(io::FsPathFromUtf8(relStr));
        const std::string srcExt = lower(relPath.extension().string());
        const std::string targetExt = formatExt.empty() ? srcExt : formatExt;

        fs::path outRel = relPath;
        outRel.replace_extension(targetExt);
        const fs::path outFile = targetDir / outRel;

        // Point the saved model at the exported file when the format changed
        // (WC3 texture paths use backslashes).
        if (targetExt != srcExt) {
            std::string nn = origName;
            if (const auto dot = nn.rfind('.'); dot != std::string::npos)
                nn.resize(dot);
            tex.fileName = nn + targetExt;
        }

        std::error_code ec;
        if (fs::exists(outFile, ec)) {
            st.skipped++;
            continue;
        }
        if (!provider) {
            st.failed++;
            continue;
        }

        std::string actualExt;
        std::optional<std::vector<u8>> bytes = provider->ReadFile(origName, &actualExt);
        if (!bytes || bytes->empty()) {
            std::fprintf(stderr, "[viewer] Export: source texture not found: %s\n",
                         origName.c_str());
            st.failed++;
            continue;
        }
        actualExt = lower(actualExt);

        std::vector<u8> outBytes;
        if (targetExt == actualExt) {
            outBytes = std::move(*bytes);
        } else {
            auto decoded = model::DispatchTextureParser(
                actualExt, [&](auto& p) { return p.parse(std::span<const u8>(*bytes)); });
            std::optional<std::vector<u8>> enc =
                decoded ? EncodeTextureAs(*decoded, targetExt) : std::nullopt;
            if (!enc) {
                std::fprintf(stderr, "[viewer] Export: convert failed %s -> %s\n",
                             origName.c_str(), targetExt.c_str());
                st.failed++;
                continue;
            }
            outBytes = std::move(*enc);
        }

        fs::create_directories(outFile.parent_path(), ec);
        std::ofstream f(outFile, std::ios::binary);
        if (f)
            f.write(reinterpret_cast<const char*>(outBytes.data()),
                    static_cast<std::streamsize>(outBytes.size()));
        if (!f) {
            st.failed++;
            continue;
        }
        st.exported++;
    }
    return st;
}

// Re-serialises the currently-loaded model to `outPath`. The Writer picks
// MDX-binary vs MDL-text from the file extension; `dialect` only matters for
// .mdl output. When `exportTextures`, the model's used textures are written next
// to it first (see ExportModelTextures) — `formatExt` optionally converts them.
// Returns false (and logs) when no model is loaded or the write throws.
bool WriteCurrentModel(ViewerApp& app, const std::string& outPath,
                       whiteout::mdx::MdlFormat dialect, bool exportTextures,
                       const std::string& formatExt) {
    // The active document's actor holds a strong ref to its template — the most
    // reliable source. Fall back to the (weak) path-keyed cache if there's no
    // focused actor.
    std::shared_ptr<model::ModelTemplate> tmpl;
    if (model::Actor* actor = app.FocusActorPtr())
        tmpl = actor->sourceTemplate;
    if (!tmpl || !tmpl->adapter)
        tmpl = app.Service().Scene().Templates().Lookup(io::PathToUtf8(app.CurrentModelPath()));
    if (!tmpl || !tmpl->adapter) {
        std::fprintf(stderr, "[viewer] Save As: no source model to write\n");
        return false;
    }
    // Copy so texture-path rewrites during export don't touch the live template.
    whiteout::mdx::Model model = tmpl->adapter->SourceModel();
    if (exportTextures) {
        const ExportStats st = ExportModelTextures(
            app, model, std::filesystem::path(io::FsPathFromUtf8(outPath)).parent_path(), formatExt);
        std::printf("[viewer] Textures: %d exported, %d skipped, %d failed\n", st.exported,
                    st.skipped, st.failed);
    }
    try {
        whiteout::mdx::Writer writer;
        writer.write(outPath, model, dialect);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[viewer] Save As FAILED '%s': %s\n", outPath.c_str(), e.what());
        return false;
    }
    std::printf("[viewer] Saved model: %s\n", outPath.c_str());
    return true;
}

// Save a standalone PopcornFX effect (.pkb / .pkfx). There's no editable model
// behind an effect — the viewer just plays it — so "save" writes the source
// bytes out verbatim.
//
// The source may not be a file on disk: documents opened from the Storage
// Explorer set CurrentModelPath() to an archive-relative path (CASC / MPQ), so
// only the on-disk case can be a plain copy. Everything else is read back
// through the document's own content provider — the same one the effect was
// loaded through — and written out.
bool WriteCurrentPkb(ViewerApp& app, const std::string& outPath) {
    const std::filesystem::path src = app.CurrentModelPath();

    std::error_code ec;
    if (std::filesystem::exists(src, ec) && !ec) {
        std::filesystem::copy_file(src, outPath,
                                   std::filesystem::copy_options::overwrite_existing, ec);
        if (!ec) {
            std::printf("[viewer] Saved effect: %s\n", outPath.c_str());
            return true;
        }
    }

    const std::string rel = io::PathToUtf8(src);
    std::optional<std::vector<u8>> bytes;
    if (io::IContentProvider* provider = app.Service().Scene().ActiveContentProvider())
        bytes = provider->ReadFile(rel);
    if (!bytes) {
        std::fprintf(stderr, "[viewer] Save As FAILED: cannot read effect '%s'\n", rel.c_str());
        return false;
    }

    std::ofstream out(std::filesystem::path(outPath), std::ios::binary);
    if (out)
        out.write(reinterpret_cast<const char*>(bytes->data()),
                  static_cast<std::streamsize>(bytes->size()));
    if (!out) {
        std::fprintf(stderr, "[viewer] Save As FAILED: cannot write '%s'\n", outPath.c_str());
        return false;
    }
    std::printf("[viewer] Saved effect: %s (%zu bytes)\n", outPath.c_str(), bytes->size());
    return true;
}

} // namespace

void ViewerUI::SaveAsDialog() {
    // The output format is locked to the source's: a PopcornFX effect
    // (.pkb/.pkfx) is copied verbatim and can only be re-saved as the same
    // effect, while a model can only be written as MDX or MDL.
    std::string srcExt = app_.CurrentModelPath().extension().string();
    for (auto& c : srcExt)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    NFD::UniquePathU8 outPath;

    if (srcExt == ".pkb" || srcExt == ".pkfx") {
        // Effects are copied byte-for-byte, so the output keeps the source
        // extension (the bytes are that format).
        const char* effExt = (srcExt == ".pkfx") ? "pkfx" : "pkb";
        nfdu8filteritem_t effFilter[1] = {{"PopcornFX effect", effExt}};
        if (NFD::SaveDialog(outPath, effFilter, 1) != NFD_OKAY)
            return;
        WriteCurrentPkb(app_, outPath.get());
        return;
    }

    // Model: two separate filter entries (not "mdx,mdl") so NFD appends the
    // right extension for whichever the user selects — that extension is then
    // how we decide binary vs text.
    nfdu8filteritem_t filter[2] = {{"MDX model (binary)", "mdx"}, {"MDL model (text)", "mdl"}};
    if (NFD::SaveDialog(outPath, filter, 2) != NFD_OKAY)
        return;

    std::string path = outPath.get();
    std::string ext = std::filesystem::path(path).extension().string();
    for (auto& c : ext)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    // Both MDX and MDL route through the options modal next frame (texture
    // export + format); MDL additionally offers the dialect choice there.
    pendingSavePath_ = std::move(path);
    pendingSaveIsMdl_ = (ext == ".mdl");
    openSaveOptionsPopup_ = true;
}

void ViewerUI::BuildSaveOptionsPopup() {
    if (openSaveOptionsPopup_) {
        ImGui::OpenPopup(i18n::tr("dialog.saveas.title"));
        openSaveOptionsPopup_ = false;
    }

    const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal(i18n::tr("dialog.saveas.title"), nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize))
        return;

    // ---- MDL dialect (text format only) ----
    if (pendingSaveIsMdl_) {
        ImGui::TextUnformatted(i18n::tr("dialog.saveas.prompt"));
        ImGui::RadioButton(i18n::tr("dialog.saveas.wc3"), &saveDialect_, 0);
        ImGui::SameLine();
        ImGui::RadioButton(i18n::tr("dialog.saveas.hive"), &saveDialect_, 1);
        ImGui::TextDisabled("%s", i18n::tr("dialog.saveas.wc3_desc"));
        ImGui::TextDisabled("%s", i18n::tr("dialog.saveas.hive_desc"));
        ImGui::Separator();
    }

    // ---- Texture export ----
    ImGui::Checkbox(i18n::tr("dialog.saveas.export_textures"), &saveExportTextures_);
    ImGui::BeginDisabled(!saveExportTextures_);
    ImGui::TextDisabled("%s", i18n::tr("dialog.saveas.export_hint"));
    saveTexFormatIdx_ =
        std::clamp(saveTexFormatIdx_, 0, static_cast<i32>(std::size(kExportFormats)) - 1);
    auto formatLabel = [&](i32 i) {
        return kExportFormats[i].ext[0] ? kExportFormats[i].label
                                        : i18n::tr("dialog.saveas.keep_original");
    };
    ImGui::SetNextItemWidth(180);
    if (ImGui::BeginCombo(i18n::tr("dialog.saveas.convert_to"), formatLabel(saveTexFormatIdx_))) {
        for (i32 i = 0; i < static_cast<i32>(std::size(kExportFormats)); ++i) {
            const bool sel = (i == saveTexFormatIdx_);
            if (ImGui::Selectable(formatLabel(i), sel))
                saveTexFormatIdx_ = i;
            if (sel)
                ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    ImGui::EndDisabled();

    ImGui::Separator();
    if (ImGui::Button(i18n::tr("app.save"), ImVec2(120, 0))) {
        const auto dialect = (saveDialect_ == 1) ? whiteout::mdx::MdlFormat::Hiveworkshop
                                                 : whiteout::mdx::MdlFormat::WarcraftIII;
        WriteCurrentModel(app_, pendingSavePath_, dialect, saveExportTextures_,
                          kExportFormats[saveTexFormatIdx_].ext);
        pendingSavePath_.clear();
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button(i18n::tr("app.cancel"), ImVec2(80, 0))) {
        pendingSavePath_.clear();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void ViewerUI::BuildExportPopup() {
    if (openExportPopup_) {
        ImGui::OpenPopup(i18n::tr("dialog.export.title"));
        openExportPopup_ = false;
    }

    const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal(i18n::tr("dialog.export.title"), nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize))
        return;

    const auto& seqs = app_.SequenceNames();
    if (seqs.empty()) {
        ImGui::TextUnformatted(i18n::tr("dialog.export.no_anims"));
        if (ImGui::Button(i18n::tr("app.close"), ImVec2(80, 0)))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }

    exportSeqIdx_ = std::clamp(exportSeqIdx_, 0, static_cast<i32>(seqs.size()) - 1);

    // ---- Animation ----
    ImGui::SetNextItemWidth(280);
    if (ImGui::BeginCombo(i18n::tr("toolbar.animation"), seqs[exportSeqIdx_].c_str())) {
        for (i32 i = 0; i < static_cast<i32>(seqs.size()); ++i) {
            const bool sel = (i == exportSeqIdx_);
            if (ImGui::Selectable(seqs[i].c_str(), sel))
                exportSeqIdx_ = i;
            if (sel)
                ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }

    // ---- FPS ----
    ImGui::SetNextItemWidth(120);
    ImGui::InputInt(i18n::tr("dialog.export.fps"), &exportFps_);
    exportFps_ = std::clamp(exportFps_, 1, 240);

    // ---- Format ----
    {
        const char* formats[kExportFormatCount];
        for (i32 i = 0; i < kExportFormatCount; ++i)
            formats[i] = GetExportFormatInfo(static_cast<ExportFormat>(i)).label;
        ImGui::SetNextItemWidth(200);
        ImGui::Combo(i18n::tr("dialog.export.format"), &exportFormat_, formats, kExportFormatCount);
    }
    const ExportFormat exportFmt = static_cast<ExportFormat>(exportFormat_);

    // ---- Resolution ----
    {
        const char* modes[] = {i18n::tr("dialog.export.res_current"),
                               i18n::tr("dialog.export.res_custom")};
        ImGui::SetNextItemWidth(200);
        ImGui::Combo(i18n::tr("dialog.export.resolution"), &exportResMode_, modes, 2);
        if (exportResMode_ == 1) {
            ImGui::SetNextItemWidth(96);
            ImGui::InputInt(i18n::tr("dialog.export.w"), &exportWidth_, 0);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(96);
            ImGui::InputInt(i18n::tr("dialog.export.h"), &exportHeight_, 0);
            exportWidth_ = std::clamp(exportWidth_, 16, 8192);
            exportHeight_ = std::clamp(exportHeight_, 16, 8192);
        }
    }

    // ---- Transparent background ----
    ImGui::Checkbox(i18n::tr("dialog.export.transparent"), &exportTransparent_);

    // ---- Capture UI overlay ----
    ImGui::Checkbox(i18n::tr("dialog.export.capture_ui"), &exportCaptureUi_);

    // ---- Output folder ----
    {
        char tmp[1024];
        std::snprintf(tmp, sizeof(tmp), "%s", exportFolder_.c_str());
        ImGui::SetNextItemWidth(360);
        if (ImGui::InputText("##exportfolder", tmp, sizeof(tmp)))
            exportFolder_ = tmp;
        ImGui::SameLine();
        if (ImGui::Button(i18n::tr("dialog.export.browse"))) {
            NFD::UniquePathU8 outPath;
            if (NFD::PickFolder(outPath) == NFD_OKAY)
                exportFolder_ = outPath.get();
        }
        ImGui::SameLine();
        ImGui::TextUnformatted(i18n::tr("dialog.export.output_folder"));
    }

    // ---- Duration / frame-count preview ----
    const auto& ranges = app_.SequenceRanges();
    if (exportSeqIdx_ < static_cast<i32>(ranges.size())) {
        const SequenceInfo& s = ranges[exportSeqIdx_];
        const i32 durMs = std::max(0, s.endMs - s.startMs);
        const i32 frames = std::max(
            1, static_cast<i32>(std::llround(static_cast<f64>(durMs) * exportFps_ / 1000.0)));
        ImGui::TextDisabled(i18n::tr("dialog.export.duration"), durMs, frames, exportFps_);
    }
    if (IsSingleFileFormat(exportFmt))
        ImGui::TextDisabled(i18n::tr("dialog.export.output_single"),
                            GetExportFormatInfo(exportFmt).extension);
    else
        ImGui::TextDisabled(i18n::tr("dialog.export.output_frames"));

    ImGui::Separator();

    const bool canExport = !exportFolder_.empty();
    ImGui::BeginDisabled(!canExport);
    if (ImGui::Button(i18n::tr("dialog.export.export"), ImVec2(120, 0))) {
        AnimationExportParams params;
        params.sequenceIndex = exportSeqIdx_;
        params.fps = exportFps_;
        params.format = exportFmt;
        params.transparentBackground = exportTransparent_;
        params.captureUi = exportCaptureUi_;
        if (exportResMode_ == 1) {
            params.width = exportWidth_;
            params.height = exportHeight_;
        }
        params.outputFolder = io::FsPathFromUtf8(exportFolder_);
        app_.RequestAnimationExport(std::move(params));
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button(i18n::tr("app.cancel"), ImVec2(80, 0)))
        ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

void ViewerUI::BuildMenuBar() {
    RenderService& svc = app_.Service();
    DisplayFlags df = svc.Settings().GetDisplayFlags();
    bool dfChanged = false;

    if (ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu(i18n::tr("menu.file"))) {
            if (ImGui::MenuItem(i18n::tr("menu.file.open"), "Ctrl+O"))
                OpenFileDialog();
            const bool hasModel = !app_.CurrentModelPath().empty();
            if (ImGui::MenuItem(i18n::tr("menu.file.save_as"), "Ctrl+Shift+S", false, hasModel))
                SaveAsDialog();
            const bool hasAnims = hasModel && !app_.SequenceNames().empty();
            if (ImGui::MenuItem(i18n::tr("menu.file.export_frames"), nullptr, false, hasAnims)) {
                model::Actor* focus = app_.FocusActorPtr();
                exportSeqIdx_ = focus ? focus->animation.ActiveSequenceIndex() : 0;
                openExportPopup_ = true;
            }
            ImGui::Separator();
            if (ImGui::MenuItem(i18n::tr("menu.file.exit")))
                glfwSetWindowShouldClose(app_.Window(), GLFW_TRUE);
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu(i18n::tr("menu.view"))) {
            dfChanged |= ImGui::MenuItem(i18n::tr("menu.view.grid"), nullptr, &df.showGrid);
            dfChanged |= ImGui::MenuItem(i18n::tr("menu.view.particles"), nullptr, &df.showParticles);
            dfChanged |= ImGui::MenuItem(i18n::tr("menu.view.ribbons"), nullptr, &df.showRibbons);
            dfChanged |= ImGui::MenuItem(i18n::tr("menu.view.events"), nullptr, &df.showEvents);
            ImGui::MenuItem(i18n::tr("menu.view.viewcube"), nullptr, &showViewCube_);

            ImGui::Separator();
            {
                // "Reforged Graphics" — force the HD pipeline for every model.
                bool reforged = app_.ForceHd();
                if (ImGui::MenuItem(i18n::tr("menu.view.reforged"), nullptr, &reforged)) {
                    app_.SetForceHd(reforged);
                    SaveIni(app_);
                }
            }

            ImGui::Separator();
            if (ImGui::BeginMenu(i18n::tr("menu.view.tileset"))) {
                const i32 n = static_cast<i32>(io::Tileset::Count);
                const i32 cur = static_cast<i32>(io::GetCurrentTileset());
                for (i32 i = 0; i < n; ++i) {
                    const bool sel = (i == cur);
                    if (ImGui::MenuItem(io::TilesetName(static_cast<io::Tileset>(i)), nullptr,
                                        sel)) {
                        svc.Replaceables().SetTileset(static_cast<io::Tileset>(i));
                        SaveIni(app_);
                    }
                }
                ImGui::EndMenu();
            }
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu(i18n::tr("menu.debug"))) {
            dfChanged |= ImGui::MenuItem(i18n::tr("menu.debug.collisions"), nullptr,
                                         &df.showCollisions);
            dfChanged |= ImGui::MenuItem(i18n::tr("menu.debug.lights"), nullptr, &df.showLights);
            ImGui::Separator();

            if (ImGui::BeginMenu(i18n::tr("menu.debug.debugview"))) {
                const i32 cur = svc.Settings().HdDebugMode();
                for (i32 i = 0; i < static_cast<i32>(kDebugVisLabels.size()); ++i) {
                    if (ImGui::MenuItem(i18n::tr(kDebugVisKeys[i]), nullptr, i == cur)) {
                        svc.Settings().SetHdDebugMode(i);
                        SaveIni(app_);
                    }
                }
                ImGui::EndMenu();
            }

            if (ImGui::BeginMenu(i18n::tr("menu.debug.lod"))) {
                const i32 cur = svc.Settings().LodOverride();
                const i32 curIdx = (cur < 0) ? 0 : (1 + std::clamp(cur, 0, 3));
                for (i32 i = 0; i < static_cast<i32>(kLodLabels.size()); ++i) {
                    if (ImGui::MenuItem(i18n::tr(kLodKeys[i]), nullptr, i == curIdx)) {
                        svc.Settings().SetLodOverride(i == 0 ? -1 : (i - 1));
                        SaveIni(app_);
                    }
                }
                ImGui::EndMenu();
            }

            ImGui::Separator();
            ImGui::MenuItem(i18n::tr("menu.debug.log_console"), nullptr, &showLogConsole_);

            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu(i18n::tr("menu.tools"))) {
            bool seOpen = app_.StorageExplorerOpen();
            if (ImGui::MenuItem(i18n::tr("menu.tools.storage_explorer"), nullptr, &seOpen))
                app_.SetStorageExplorerOpen(seOpen);
            ImGui::EndMenu();
        }

        // Language picker — endonyms are shown in their own script (not
        // translated); the bundled Noto fonts cover every entry. Switching only
        // swaps the in-memory catalog, so the whole UI re-localizes next frame.
        if (ImGui::BeginMenu(i18n::tr("menu.language"))) {
            const i18n::Language cur = i18n::Localizer::instance().current();
            for (const auto& e : i18n::languages()) {
                if (ImGui::MenuItem(e.endonym, nullptr, e.lang == cur)) {
                    i18n::Localizer::instance().setLanguage(e.lang);
                    SaveIni(app_);
                }
            }
            ImGui::EndMenu();
        }

        if (ImGui::MenuItem(i18n::tr("menu.settings")))
            settingsOpen_ = true;

        ImGui::EndMainMenuBar();
    }

    if (dfChanged) {
        svc.Settings().SetDisplayFlags(df);
        SaveIni(app_);
    }
}

void ViewerUI::BuildToolbar() {
    // Anchor the toolbar just below the main menu bar; sized to the
    // viewport width, fixed-height. No close / collapse decorations.
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const f32 menuH = ImGui::GetFrameHeight();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x, vp->WorkPos.y));
    ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x, menuH + 8.0f));
    ImGuiWindowFlags wf = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                          ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings |
                          ImGuiWindowFlags_NoBringToFrontOnFocus;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    if (!ImGui::Begin("##toolbar", nullptr, wf)) {
        ImGui::End();
        ImGui::PopStyleVar(2);
        return;
    }

    RenderService& svc = app_.Service();

    // ---- Animation sequence ----
    const auto& seqs = app_.SequenceNames();
    if (!seqs.empty()) {
        model::Actor* focus = app_.FocusActorPtr();
        i32 sel = focus ? focus->animation.ActiveSequenceIndex() : 0;
        ImGui::SetNextItemWidth(220);
        if (ImGui::BeginCombo(i18n::tr("toolbar.animation"),
                              seqs[std::clamp(sel, 0, (i32)seqs.size() - 1)].c_str())) {
            for (i32 i = 0; i < static_cast<i32>(seqs.size()); ++i) {
                const bool isSel = (i == sel);
                if (ImGui::Selectable(seqs[i].c_str(), isSel)) {
                    if (focus) {
                        const i32 prev = focus->animation.ActiveSequenceIndex();
                        focus->animation.SetActiveSequenceIndex(i);
                        if (i != prev) {
                            const std::string& name = seqs[i];
                            const bool keep = (name.find("decay") != std::string::npos) ||
                                              (name.find("dissipate") != std::string::npos);
                            if (!keep)
                                svc.Splats().Clear();
                        }
                    }
                }
                if (isSel)
                    ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
    }

    // ---- Camera preset ----
    {
        const auto& presetNames = app_.CameraPresetNamesUtf8();
        const i32 active = app_.ActiveCameraPresetIdx();
        const char* preview = (active < 0 || active >= static_cast<i32>(presetNames.size()))
                                  ? i18n::tr("toolbar.camera.free")
                                  : presetNames[active].c_str();
        ImGui::SetNextItemWidth(140);
        if (ImGui::BeginCombo(i18n::tr("toolbar.camera"), preview)) {
            if (ImGui::Selectable(i18n::tr("toolbar.camera.free"), active < 0))
                app_.ActivateCameraPreset(-1);
            for (i32 i = 0; i < static_cast<i32>(presetNames.size()); ++i) {
                const bool isSel = (i == active);
                if (ImGui::Selectable(presetNames[i].c_str(), isSel))
                    app_.ActivateCameraPreset(i);
                if (isSel)
                    ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
    }

    // ---- Team colour ----
    {
        model::Actor* focus = app_.FocusActorPtr();
        u32 tcRaw = focus ? (focus->teamColor & 0x00FFFFFFu) : 0x000000FFu;
        f32 col[3] = {
            static_cast<f32>(tcRaw & 0xFFu) / 255.0f,
            static_cast<f32>((tcRaw >> 8) & 0xFFu) / 255.0f,
            static_cast<f32>((tcRaw >> 16) & 0xFFu) / 255.0f,
        };
        ImGuiColorEditFlags flags = ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel;
        ImGui::TextUnformatted(i18n::tr("toolbar.team"));
        ImGui::SameLine();
        if (ImGui::ColorEdit3("##team", col, flags)) {
            if (focus) {
                focus->SetTeamColor(static_cast<u8>(col[0] * 255.0f),
                                    static_cast<u8>(col[1] * 255.0f),
                                    static_cast<u8>(col[2] * 255.0f));
            }
        }
        ImGui::SameLine();
    }

    // ---- Lighting mode ----
    {
        i32 sel = static_cast<i32>(svc.Settings().GetLightingMode());
        ImGui::SetNextItemWidth(120);
        const char* lightingItems[3];
        for (i32 i = 0; i < static_cast<i32>(kLightingKeys.size()); ++i)
            lightingItems[i] = i18n::tr(kLightingKeys[i]);
        if (ImGui::Combo(i18n::tr("toolbar.lighting"), &sel, lightingItems,
                         static_cast<i32>(kLightingLabels.size()))) {
            svc.Settings().SetLightingMode(static_cast<LightingMode>(sel));
            SaveIni(app_);
        }
    }

    ImGui::End();
    ImGui::PopStyleVar(2);
}

void ViewerUI::BuildTabBar() {
    const i32 count = app_.DocumentCount();
    if (count <= 0)
        return; // nothing open — no strip

    // Anchor a thin strip directly beneath the toolbar (which is menuH + 8 tall
    // and sits at WorkPos.y). The tab bar draws over the top of the 3D view.
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const f32 menuH = ImGui::GetFrameHeight();
    const f32 toolbarH = menuH + 8.0f;
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x, vp->WorkPos.y + toolbarH));
    ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x, menuH + 8.0f));
    ImGuiWindowFlags wf = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                          ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings |
                          ImGuiWindowFlags_NoBringToFrontOnFocus;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    if (!ImGui::Begin("##tabbar", nullptr, wf)) {
        ImGui::End();
        ImGui::PopStyleVar(2);
        return;
    }

    ImGuiTabBarFlags tbFlags = ImGuiTabBarFlags_AutoSelectNewTabs |
                               ImGuiTabBarFlags_Reorderable |
                               ImGuiTabBarFlags_FittingPolicyScroll;
    if (ImGui::BeginTabBar("##documents", tbFlags)) {
        // After an app-driven active change (CLI bulk-load, File > Open, a
        // close handing focus to a neighbour) the tab bar would otherwise
        // default to its first tab. Force-select the app's active tab for that
        // one frame, and skip the "follow ImGui's selection" logic so a
        // transient first-tab selection can't snap the active document back.
        const i32 forceSelect = app_.ConsumePendingTabSelect();
        i32 toClose = -1;
        for (i32 i = 0; i < app_.DocumentCount(); ++i) {
            bool open = true;
            const ImGuiTabItemFlags flags =
                (i == forceSelect) ? ImGuiTabItemFlags_SetSelected : 0;
            // PushID disambiguates tabs whose labels (file stems) collide; the
            // visible label is still the file name.
            ImGui::PushID(i);
            if (ImGui::BeginTabItem(app_.DocumentTitle(i).c_str(), &open, flags)) {
                // BeginTabItem returns true for the selected tab — follow the
                // user's click by activating that document (but not on a
                // force-select frame, where ImGui's selection is still settling).
                if (forceSelect < 0 && app_.ActiveDocumentIndex() != i)
                    app_.SetActiveDocument(i);
                ImGui::EndTabItem();
            }
            ImGui::PopID();
            if (!open)
                toClose = i; // the (x) was clicked
        }
        ImGui::EndTabBar();
        if (toClose >= 0)
            app_.CloseDocument(toClose);
    }

    ImGui::End();
    ImGui::PopStyleVar(2);
}

void ViewerUI::BuildSettingsWindow() {
    RenderService& svc = app_.Service();
    ImGui::SetNextWindowSize(ImVec2(440, 540), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(i18n::tr("settings.title"), &settingsOpen_)) {
        ImGui::End();
        return;
    }

    if (!ImGui::BeginTabBar("##SettingsTabs")) {
        ImGui::End();
        return;
    }

    if (ImGui::BeginTabItem(i18n::tr("settings.tab.general"))) {
        // ---- Background colour ----
        {
            const u32 bg = svc.Settings().BackgroundColorRaw();
            f32 col[3] = {
                static_cast<f32>(bg & 0xFFu) / 255.0f,
                static_cast<f32>((bg >> 8) & 0xFFu) / 255.0f,
                static_cast<f32>((bg >> 16) & 0xFFu) / 255.0f,
            };
            if (ImGui::ColorEdit3(i18n::tr("settings.general.background"), col)) {
                svc.Settings().SetBackgroundColor(static_cast<u8>(col[0] * 255.0f),
                                                  static_cast<u8>(col[1] * 255.0f),
                                                  static_cast<u8>(col[2] * 255.0f));
                SaveIni(app_);
            }
        }

        // ---- Exposure ----
        {
            f32 exposure = svc.Settings().GetTonemapExposure();
            if (ImGui::SliderFloat(i18n::tr("settings.general.exposure"), &exposure, 0.0f, 3.0f,
                                   "%.2f")) {
                svc.Settings().SetTonemapExposure(exposure);
                SaveIni(app_);
            }
        }

        // ---- Sound volume ----
        {
            f32 vol = svc.Sound().GetVolume();
            if (ImGui::SliderFloat(i18n::tr("settings.general.snd_volume"), &vol, 0.0f, 1.0f,
                                   "%.2f")) {
                svc.Sound().SetVolume(vol);
                SaveIni(app_);
            }
        }

        // ---- Loop non-looping ----
        {
            bool on = app_.LoopNonLoopingPolicy();
            if (ImGui::Checkbox(i18n::tr("settings.general.loop_nonlooping"), &on)) {
                app_.SetLoopNonLoopingPolicy(on);
                SaveIni(app_);
            }
        }

        ImGui::Separator();

        // ---- Time of day ----
        if (auto* dnc = svc.GetDncService()) {
            const f32 hpd = dnc->GetHoursPerDay();
            f32 tod = dnc->GetTimeOfDay();
            if (ImGui::SliderFloat(i18n::tr("settings.general.time_of_day"), &tod, 0.0f, hpd,
                                   "%.2f h")) {
                dnc->SetTimeOfDay(tod);
                SaveIni(app_);
            }
            bool animating = dnc->GetTodScale() > 0.0f;
            if (ImGui::Checkbox(i18n::tr("settings.general.animate_tod"), &animating)) {
                dnc->SetTodScale(animating ? 1.0f : 0.0f);
                SaveIni(app_);
            }
        }

        ImGui::Separator();

        // ---- IBL mode ----
        {
            i32 sel = static_cast<i32>(svc.Settings().GetIblMode());
            const char* iblItems[4];
            for (i32 i = 0; i < static_cast<i32>(kIblKeys.size()); ++i)
                iblItems[i] = i18n::tr(kIblKeys[i]);
            if (ImGui::Combo(i18n::tr("settings.general.ibl"), &sel, iblItems,
                             static_cast<i32>(kIblLabels.size()))) {
                svc.Settings().SetIblMode(static_cast<IblMode>(sel));
                SaveIni(app_);
            }
        }

        // ---- Shadows ----
        {
            i32 sel = 0;
            if (auto* shadow = svc.GetShadowService()) {
                sel = shadow->IsEnabled() ? std::clamp(shadow->Params().cascadeCount, 0, 3) : 0;
            }
            const char* shadowItems[4];
            for (i32 i = 0; i < static_cast<i32>(kShadowKeys.size()); ++i)
                shadowItems[i] = i18n::tr(kShadowKeys[i]);
            if (ImGui::Combo(i18n::tr("settings.general.shadows"), &sel, shadowItems,
                             static_cast<i32>(kShadowLabels.size()))) {
                if (auto* shadow = svc.GetShadowService()) {
                    shadow::ShadowParams p = shadow->Params();
                    p.enabled = (sel > 0);
                    p.cascadeCount = (sel > 0) ? sel : 1;
                    shadow->SetParams(p);
                    SaveIni(app_);
                }
            }
        }

        // ---- Ambient occlusion (GTAO) ----
        {
            bool ao = svc.Settings().AoEnabled();
            if (ImGui::Checkbox(i18n::tr("settings.general.ao"), &ao)) {
                svc.Settings().SetAoEnabled(ao);
                SaveIni(app_);
            }

            static constexpr std::array<const char*, 3> kAoQualityLabels = {"Low", "Medium",
                                                                            "High"};
            static constexpr std::array<const char*, 3> kAoQualityKeys = {
                "aoquality.low", "aoquality.medium", "aoquality.high"};
            i32 q = static_cast<i32>(svc.Settings().AoQuality());
            if (q < 0 || q >= static_cast<i32>(kAoQualityLabels.size()))
                q = 1;
            ImGui::SetNextItemWidth(180.0f);
            const char* aoItems[3];
            for (i32 i = 0; i < static_cast<i32>(kAoQualityKeys.size()); ++i)
                aoItems[i] = i18n::tr(kAoQualityKeys[i]);
            if (ImGui::Combo(i18n::tr("settings.general.ao_quality"), &q, aoItems,
                             static_cast<i32>(kAoQualityLabels.size()))) {
                svc.Settings().SetAoQuality(static_cast<u32>(q));
                SaveIni(app_);
            }

            f32 boost = svc.Settings().AoBentBoost();
            ImGui::SetNextItemWidth(180.0f);
            if (ImGui::SliderFloat(i18n::tr("settings.general.ao_bent_boost"), &boost, 0.0f, 0.5f,
                                   "%.3f")) {
                svc.Settings().SetAoBentBoost(boost);
                SaveIni(app_);
            }
        }

        // ---- Bloom (HD-only) ----
        // CollapsingHeader keeps three sliders + a reset button from
        // crowding the main settings list when bloom is off. Defaults
        // mirror the engine's RegisterBloom (BL_BLOOM_D=off,
        // threshold=1.0, intensity=1.25, saturation=1.0).
        if (ImGui::CollapsingHeader(i18n::tr("settings.bloom.header"))) {
            bool bloom = svc.Settings().BloomEnabled();
            if (ImGui::Checkbox(i18n::tr("settings.bloom.enabled"), &bloom)) {
                svc.Settings().SetBloomEnabled(bloom);
                SaveIni(app_);
            }
            ImGui::SameLine();
            if (ImGui::SmallButton(i18n::tr("settings.bloom.reset"))) {
                svc.Settings().SetBloomThreshold(1.0f);
                svc.Settings().SetBloomIntensity(1.25f);
                svc.Settings().SetBloomSaturation(1.0f);
                SaveIni(app_);
            }
            f32 threshold = svc.Settings().BloomThreshold();
            f32 intensity = svc.Settings().BloomIntensity();
            f32 saturation = svc.Settings().BloomSaturation();
            ImGui::SetNextItemWidth(180.0f);
            if (ImGui::SliderFloat(i18n::tr("settings.bloom.threshold"), &threshold, 0.0f, 4.0f,
                                   "%.2f")) {
                svc.Settings().SetBloomThreshold(threshold);
                SaveIni(app_);
            }
            ImGui::SetNextItemWidth(180.0f);
            if (ImGui::SliderFloat(i18n::tr("settings.bloom.intensity"), &intensity, 0.0f, 4.0f,
                                   "%.2f")) {
                svc.Settings().SetBloomIntensity(intensity);
                SaveIni(app_);
            }
            ImGui::SetNextItemWidth(180.0f);
            if (ImGui::SliderFloat(i18n::tr("settings.bloom.saturation"), &saturation, 0.0f, 4.0f,
                                   "%.2f")) {
                svc.Settings().SetBloomSaturation(saturation);
                SaveIni(app_);
            }
        }

        // ---- Depth of Field (HD-only) ----
        // Runs the shipped depthoffield.bls. The pass self-disables until a
        // focal distance > 0 is set, so enabling with a zero distance seeds a
        // sensible default — otherwise the checkbox would appear to do nothing.
        if (ImGui::CollapsingHeader(i18n::tr("settings.dof.header"))) {
            // Focus on the subject: the camera→target distance is the model
            // centre's view-space depth, which is what `linearDepth` carries.
            // The CoC is hyperbolic — (1/focus − 1/depth)·focusScale — so at
            // view-space depths (hundreds) focusScale needs to be ~tens-hundreds
            // for visible blur, not the ~1 a normalised-depth pass would use.
            const f32 camDist = svc.Scene().Camera().GetDistance();
            const f32 autoFocus = camDist > 0.0f ? camDist : 600.0f;
            bool dof = svc.Settings().DofEnabled();
            if (ImGui::Checkbox(i18n::tr("settings.dof.enabled"), &dof)) {
                svc.Settings().SetDofEnabled(dof);
                if (dof) {
                    // Auto-focus on the model and seed a visible strength if the
                    // current values would produce no perceptible blur.
                    if (svc.Settings().DofFocusDistance() <= 0.0f)
                        svc.Settings().SetDofFocusDistance(autoFocus);
                    if (svc.Settings().DofFocusScale() < 5.0f)
                        svc.Settings().SetDofFocusScale(50.0f);
                }
                SaveIni(app_);
            }
            ImGui::SameLine();
            if (ImGui::SmallButton(i18n::tr("settings.dof.reset"))) {
                svc.Settings().SetDofFocusDistance(autoFocus);
                svc.Settings().SetDofFocusScale(50.0f);
                svc.Settings().SetDofMaxBlurSize(20.0f);
                svc.Settings().SetDofRadiusScale(1.0f);
                svc.Settings().SetDofFarFieldOnly(false);
                SaveIni(app_);
            }
            f32 focusDist = svc.Settings().DofFocusDistance();
            f32 focusScale = svc.Settings().DofFocusScale();
            f32 maxBlur = svc.Settings().DofMaxBlurSize();
            f32 radius = svc.Settings().DofRadiusScale();
            bool farOnly = svc.Settings().DofFarFieldOnly();
            ImGui::SetNextItemWidth(180.0f);
            if (ImGui::SliderFloat(i18n::tr("settings.dof.focus_dist"), &focusDist, 0.0f, 3000.0f,
                                   "%.0f")) {
                svc.Settings().SetDofFocusDistance(focusDist);
                SaveIni(app_);
            }
            ImGui::SetNextItemWidth(180.0f);
            if (ImGui::SliderFloat(i18n::tr("settings.dof.focus_scale"), &focusScale, 0.0f, 200.0f,
                                   "%.1f")) {
                svc.Settings().SetDofFocusScale(focusScale);
                SaveIni(app_);
            }
            ImGui::SetNextItemWidth(180.0f);
            if (ImGui::SliderFloat(i18n::tr("settings.dof.max_blur"), &maxBlur, 1.0f, 40.0f,
                                   "%.1f")) {
                svc.Settings().SetDofMaxBlurSize(maxBlur);
                SaveIni(app_);
            }
            ImGui::SetNextItemWidth(180.0f);
            if (ImGui::SliderFloat(i18n::tr("settings.dof.sample_density"), &radius, 0.25f, 4.0f,
                                   "%.2f")) {
                svc.Settings().SetDofRadiusScale(radius);
                SaveIni(app_);
            }
            if (ImGui::Checkbox(i18n::tr("settings.dof.far_field_only"), &farOnly)) {
                svc.Settings().SetDofFarFieldOnly(farOnly);
                SaveIni(app_);
            }
        }

        ImGui::Separator();

        // ---- DNC model ----
        // The stock DNC set is small and fully enumerable (dnc_catalog.h), so
        // this is two combos instead of a free-text path: which light rig, and
        // which mod layer to read it from. A path the catalog doesn't know —
        // an older ini, or a hand-edited one — still shows and still loads.
        if (auto* dnc = svc.GetDncService()) {
            const auto catalog = dnc::DncCatalog();
            const std::string current = dnc->UnitMdlPath();
            const i32 sel = dnc::DncCatalogIndexOf(current);
            const dnc::DncVariant variant = dnc::DncVariantOf(current);

            ImGui::SetNextItemWidth(220.0f);
            const std::string preview = (sel >= 0) ? dnc::DncEntryLabel(catalog[sel]) : current;
            if (ImGui::BeginCombo(i18n::tr("settings.general.dnc_model"), preview.c_str())) {
                for (usize i = 0; i < catalog.size(); ++i) {
                    const auto& e = catalog[i];
                    if (ImGui::Selectable(dnc::DncEntryLabel(e).c_str(),
                                          static_cast<i32>(i) == sel)) {
                        dnc->SetUnitMdl(dnc::DncPathForVariant(e.path, variant));
                        SaveIni(app_);
                    }
                    if (ImGui::IsItemHovered()) {
                        std::string tilesets;
                        for (const auto& ts : dnc::DncTilesets()) {
                            if (ts.family != e.family)
                                continue;
                            if (!tilesets.empty())
                                tilesets += ", ";
                            tilesets += std::string(ts.name);
                        }
                        ImGui::SetTooltip("%.*s\n%s %s", static_cast<i32>(e.path.size()),
                                          e.path.data(), i18n::tr("settings.general.dnc_tilesets"),
                                          tilesets.c_str());
                    }
                }
                ImGui::EndCombo();
            }
            ImGui::SameLine();
            if (ImGui::Button(i18n::tr("settings.general.dnc_reset"))) {
                dnc->SetUnitMdl(dnc::DncService::kDefaultUnitMdl);
                SaveIni(app_);
            }

            // Auto follows the provider's HD-mode mod chain; SD/HD pin the
            // path to one layer. Only Lordaeron's legacy target rig is
            // SD-only, so the HD entry is greyed out rather than hidden.
            const bool hasSd = sel < 0 || catalog[sel].hasSd;
            const bool hasHd = sel < 0 || catalog[sel].hasHd;
            const char* variantLabels[] = {i18n::tr("settings.general.dnc_variant_auto"), "SD",
                                           "HD"};
            ImGui::SetNextItemWidth(220.0f);
            if (ImGui::BeginCombo(i18n::tr("settings.general.dnc_variant"),
                                  variantLabels[static_cast<usize>(variant)])) {
                const bool enabled[] = {true, hasSd, hasHd};
                for (usize i = 0; i < std::size(variantLabels); ++i) {
                    ImGui::BeginDisabled(!enabled[i]);
                    if (ImGui::Selectable(variantLabels[i], i == static_cast<usize>(variant))) {
                        dnc->SetUnitMdl(
                            dnc::DncPathForVariant(current, static_cast<dnc::DncVariant>(i)));
                        SaveIni(app_);
                    }
                    ImGui::EndDisabled();
                }
                ImGui::EndCombo();
            }
        }

        ImGui::Separator();
        ImGui::TextDisabled(i18n::tr("settings.general.startup_note"));

        // ---- Default backend ----
        // Platform availability:
        //   Windows: D3D11, D3D12, Vulkan, WebGPU (when WDX_HAS_WEBGPU)
        //   macOS:   Vulkan, WebGPU (when WDX_HAS_WEBGPU) — D3D11/D3D12 are
        //            WIN32-only via CMake + gfx_factory
        //   Linux:   Vulkan only — D3D11/D3D12 WIN32-only, WebGPU/Dawn isn't
        //            wired into Linux builds.
#if defined(_WIN32)
        {
            // Metal is Apple-only, so it's excluded from the Windows list. It's
            // the trailing entry in kBackendLabels (index 4), so the windowed
            // set is just the first four: D3D11, D3D12, Vulkan, WebGPU.
            constexpr i32 kWinBackendCount = static_cast<i32>(kBackendLabels.size()) - 1;
            i32 sel = BackendToIdx(svc.Settings().DefaultBackend());
            if (sel >= kWinBackendCount)
                sel = BackendToIdx(gfx::GfxApi::D3D12); // clamp a stale Metal selection
            if (ImGui::Combo(i18n::tr("settings.general.backend"), &sel, kBackendLabels.data(),
                             kWinBackendCount)) {
                svc.Settings().SetDefaultBackend(IdxToBackend(sel));
                SaveIni(app_);
            }
        }
#elif defined(__APPLE__)
        {
#if WDX_HAS_WEBGPU
            const char* macLabels[] = {"Metal", "Vulkan", "WebGPU"};
            const gfx::GfxApi macApis[] = {gfx::GfxApi::Metal, gfx::GfxApi::Vulkan,
                                           gfx::GfxApi::WebGPU};
#else
            const char* macLabels[] = {"Metal", "Vulkan"};
            const gfx::GfxApi macApis[] = {gfx::GfxApi::Metal, gfx::GfxApi::Vulkan};
#endif
            // Find the index of the currently-selected backend; fall back to
            // Metal (entry 0) if the saved value is something this build
            // doesn't expose.
            const auto cur = svc.Settings().DefaultBackend();
            i32 sel = 0;
            for (i32 i = 0; i < static_cast<i32>(std::size(macApis)); ++i) {
                if (macApis[i] == cur) {
                    sel = i;
                    break;
                }
            }
            if (ImGui::Combo(i18n::tr("settings.general.backend"), &sel, macLabels,
                             static_cast<i32>(std::size(macLabels)))) {
                svc.Settings().SetDefaultBackend(macApis[sel]);
                SaveIni(app_);
            }
        }
#else
        {
            ImGui::BeginDisabled();
            i32 sel = 0;
            const char* vkOnly[] = {"Vulkan"};
            ImGui::Combo(i18n::tr("settings.general.backend"), &sel, vkOnly, 1);
            ImGui::EndDisabled();
        }
#endif

        // ---- Preferred device ----
        {
            static std::vector<std::string> devices;
            static i32 lastBackendIdx = -1;
            const i32 curBackendIdx = BackendToIdx(svc.Settings().DefaultBackend());
            if (curBackendIdx != lastBackendIdx) {
                devices = gfx::EnumerateDevices(svc.Settings().DefaultBackend());
                lastBackendIdx = curBackendIdx;
            }
            const std::string& cur = svc.Settings().PreferredDevice();
            const char* preview = cur.empty() ? i18n::tr("settings.general.device_auto") : cur.c_str();
            if (ImGui::BeginCombo(i18n::tr("settings.general.device"), preview)) {
                if (ImGui::Selectable(i18n::tr("settings.general.device_auto"), cur.empty())) {
                    svc.Settings().SetPreferredDevice("");
                    SaveIni(app_);
                }
                for (const auto& n : devices) {
                    const bool isSel = (n == cur);
                    if (ImGui::Selectable(n.c_str(), isSel)) {
                        svc.Settings().SetPreferredDevice(n);
                        SaveIni(app_);
                    }
                    if (isSel)
                        ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
        }

        // ---- Graphics debug ----
        {
            bool on = svc.Settings().GraphicsDebug();
            if (ImGui::Checkbox(i18n::tr("settings.general.graphics_debug"), &on)) {
                svc.Settings().SetGraphicsDebug(on);
                SaveIni(app_);
            }
        }

        ImGui::EndTabItem();
    }

    // ---- IO tab ----
    // Install path + ignore-flags + an editable MPQ load order. All three
    // commit to ini through SaveIoPathOverrides so the changes survive across
    // launches; the provider itself is mutated in place so the effect is live
    // (next ReadFile sees the new state).
    if (ImGui::BeginTabItem(i18n::tr("settings.tab.io"))) {
        // Every document scene shares the DEFAULT scene's provider (see
        // ViewerApp::SharedProvider). Target it directly rather than the active
        // scene's, whose own internal provider is never configured.
        auto& provider = svc.DefaultScene().GetContentProvider();
        if (!ioBufsInitialised_) {
            installPathBuf_ = provider.InstallPath();
            ioBufsInitialised_ = true;
        }

        const std::string& autoDetected = provider.Wc3Path();
        if (autoDetected.empty())
            ImGui::TextDisabled(i18n::tr("settings.io.not_detected"));
        else
            ImGui::TextDisabled(i18n::tr("settings.io.auto_detected"), autoDetected.c_str());
        ImGui::Spacing();

        // Commit the entire IO state (install path + flags + list) to ini, then
        // retry any asset that failed to load under the previous sources — the
        // provider now resolves paths differently, so textures/models that 404'd
        // (and are stuck on the placeholder) get another fetch on the next pump,
        // no restart needed.
        auto saveIo = [&] {
            IoPathOverrides o;
            // Treat "install path == auto-detected" as "no override" so the
            // ini stays clean and a future auto-detect (e.g. user installs WC3
            // in a different place) is picked up.
            o.installPath =
                (installPathBuf_ == provider.Wc3Path()) ? std::string{} : installPathBuf_;
            o.ignoreCasc = provider.IgnoreCasc();
            o.ignoreMpq = provider.IgnoreMpq();
            o.mpqListSet = true;
            o.mpqList = provider.MpqList();
            SaveIoPathOverrides(o);
            svc.RetryUnloadedAssets();
        };

        // ---- Install path row ----
        {
            char tmp[1024];
            std::snprintf(tmp, sizeof(tmp), "%s", installPathBuf_.c_str());
            ImGui::SetNextItemWidth(-180.0f);
            if (ImGui::InputText("##install", tmp, sizeof(tmp)))
                installPathBuf_ = tmp;
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                provider.SetInstallPath(installPathBuf_);
                saveIo();
            }
            ImGui::SameLine();
            if (ImGui::Button(i18n::tr("settings.io.browse_install"))) {
                NFD::UniquePathU8 outPath;
                if (NFD::PickFolder(outPath) == NFD_OKAY) {
                    installPathBuf_ = outPath.get();
                    provider.SetInstallPath(installPathBuf_);
                    saveIo();
                }
            }
            ImGui::SameLine();
            if (ImGui::Button(i18n::tr("settings.io.reset_install"))) {
                provider.SetInstallPath("");
                installPathBuf_ = provider.InstallPath();
                saveIo();
            }
            ImGui::SameLine();
            ImGui::TextUnformatted(i18n::tr("settings.io.install_path"));
        }

        ImGui::Spacing();
        ImGui::Separator();

        // ---- Ignore flags ----
        {
            bool ignoreCasc = provider.IgnoreCasc();
            if (ImGui::Checkbox(i18n::tr("settings.io.ignore_casc"), &ignoreCasc)) {
                provider.SetIgnoreCasc(ignoreCasc);
                saveIo();
            }
            bool ignoreMpq = provider.IgnoreMpq();
            if (ImGui::Checkbox(i18n::tr("settings.io.ignore_mpq"), &ignoreMpq)) {
                provider.SetIgnoreMpq(ignoreMpq);
                saveIo();
            }
        }

        ImGui::Spacing();
        ImGui::Separator();

        // ---- MPQ load list ----
        // Earlier entries win. Buttons mutate the provider's vector in place
        // (via SetMpqList(...)) which reopens the storages each time — fine
        // for a settings dialog (low-frequency edits).
        ImGui::TextUnformatted(i18n::tr("settings.io.mpq_header"));
        ImGui::BeginDisabled(provider.IgnoreMpq());

        std::vector<std::string> mpqs = provider.MpqList();
        bool mpqsDirty = false;
        i32 swapWith = -1; // [i, i+1] to swap when set
        i32 removeAt = -1;
        for (usize i = 0; i < mpqs.size(); ++i) {
            ImGui::PushID(static_cast<int>(i));
            const bool isFirst = (i == 0);
            const bool isLast = (i + 1 == mpqs.size());
            ImGui::BeginDisabled(isFirst);
            if (ImGui::ArrowButton("up", ImGuiDir_Up))
                swapWith = static_cast<i32>(i) - 1;
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::BeginDisabled(isLast);
            if (ImGui::ArrowButton("down", ImGuiDir_Down))
                swapWith = static_cast<i32>(i);
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button("X"))
                removeAt = static_cast<i32>(i);
            ImGui::SameLine();
            ImGui::TextUnformatted(mpqs[i].c_str());
            ImGui::PopID();
        }
        if (swapWith >= 0 && swapWith + 1 < static_cast<i32>(mpqs.size())) {
            std::swap(mpqs[swapWith], mpqs[swapWith + 1]);
            mpqsDirty = true;
        }
        if (removeAt >= 0 && removeAt < static_cast<i32>(mpqs.size())) {
            mpqs.erase(mpqs.begin() + removeAt);
            mpqsDirty = true;
        }

        // Add-new row.
        {
            char tmp[256];
            std::snprintf(tmp, sizeof(tmp), "%s", newMpqEntryBuf_.c_str());
            ImGui::SetNextItemWidth(-140.0f);
            if (ImGui::InputText("##newmpq", tmp, sizeof(tmp)))
                newMpqEntryBuf_ = tmp;
            ImGui::SameLine();
            const bool canAdd = !newMpqEntryBuf_.empty();
            ImGui::BeginDisabled(!canAdd);
            if (ImGui::Button(i18n::tr("settings.io.add_mpq"))) {
                mpqs.push_back(newMpqEntryBuf_);
                newMpqEntryBuf_.clear();
                mpqsDirty = true;
            }
            ImGui::EndDisabled();
        }

        if (ImGui::SmallButton(i18n::tr("settings.io.reset_defaults"))) {
            mpqs = io::FileContentProvider::DefaultMpqList();
            mpqsDirty = true;
        }

        ImGui::EndDisabled(); // IgnoreMpq guard around the list controls

        if (mpqsDirty) {
            provider.SetMpqList(std::move(mpqs));
            saveIo();
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::TextDisabled(i18n::tr("settings.io.casc_status"),
                            provider.HasCasc() ? i18n::tr("settings.io.open")
                                               : i18n::tr("settings.io.not_loaded"));
        ImGui::TextDisabled(i18n::tr("settings.io.mpq_status"),
                            provider.HasMpq() ? i18n::tr("app.yes") : i18n::tr("app.no"));

        ImGui::EndTabItem();
    }

    ImGui::EndTabBar();
    ImGui::End();
}

} // namespace whiteout::flakes
