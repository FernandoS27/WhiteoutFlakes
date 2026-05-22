#include "viewer_ui.h"

#include "imgui_viewcube.h"
#include "renderer/assets/replaceable_texture_manager.h"
#include "renderer/camera.h"
#include "renderer/debug/debug_renderer.h"
#include "io/mdx_model_adapter.h"
#include "renderer/dnc/dnc_service.h"
#include "renderer/model/model_instance.h"
#include "renderer/model/model_template.h"
#include "renderer/particle/splat_service.h"
#include "renderer/render_service.h"
#include "renderer/scene_manager.h"
#include "renderer/shadow/shadow_service.h"
#include "settings_ini.h"
#include "viewer_app.h"

#include <whiteout/models/mdx/writer.h>
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
#include <string>

namespace whiteout::flakes {

namespace {

// Persists current host state to WhiteoutFlakes.ini. Called after every UI
// change that should survive a restart — same call shape as the old
// HandleSettingsMessage paths used.
void SaveIni(const ViewerApp& app) {
    SaveSettingsIni(app.Service(), app.LoopNonLoopingPolicy());
}

constexpr std::array<const char*, 8> kDebugVisLabels = {
    "Off",
    "Albedo",
    "World Normal",
    "LOD Heatmap",
    "Light Count",
    "Shading Only (white albedo)",
    "Shading Only (grey albedo)",
    "Specular Only (black albedo)",
};

constexpr std::array<const char*, 5> kLodLabels = {
    "Auto (screen size)",  "Force LOD 0 (base)", "Force LOD 1",
    "Force LOD 2",         "Force LOD 3 (lowest)",
};

constexpr std::array<const char*, 4> kIblLabels = {"Portrait", "Day/Night", "Dungeon", "Sunset"};
constexpr std::array<const char*, 3> kLightingLabels = {"InGame", "Glue", "Dynamic"};
constexpr std::array<const char*, 4> kShadowLabels = {"Off", "1 cascade", "2 cascades",
                                                     "3 cascades"};
constexpr std::array<const char*, 4> kBackendLabels = {"D3D11", "D3D12", "Vulkan", "WebGPU"};

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
    if (showViewCube_)
        BuildViewCubeWidget();
    if (settingsOpen_)
        BuildSettingsWindow();
    BuildSaveAsPopup();
    BuildExportPopup();
}

void ViewerUI::BuildViewCubeWidget() {
    // The view-cube is a pure host-side ImGui widget — the renderer has no
    // notion of it. See tools/common/imgui_viewcube.h.
    tools::DrawViewCube(app_.Service().Scene().Camera());
}

void ViewerUI::OpenFileDialog() {
    // Use the UTF-8 NFD entry points so the filter strings stay as plain
    // `char` literals on every platform — the native variant takes wchar_t
    // on Windows, which would break these inline string constants.
    NFD::UniquePathU8 outPath;
    nfdu8filteritem_t filter[1] = {{"Warcraft III Model", "mdx,mdl"}};
    if (NFD::OpenDialog(outPath, filter, 1) == NFD_OKAY) {
        std::filesystem::path p = io::FsPathFromUtf8(outPath.get());
        app_.LoadModel(p);
    }
}

namespace {

// Re-serialises the currently-loaded model to `outPath`. The Writer picks
// MDX-binary vs MDL-text from the file extension; `dialect` only matters for
// .mdl output. Returns false (and logs) when no model is loaded or the write
// throws.
bool WriteCurrentModel(ViewerApp& app, const std::string& outPath,
                       whiteout::mdx::MdlFormat dialect) {
    auto tmpl = app.Service().Scene().Templates().Lookup(
        io::PathToUtf8(app.CurrentModelPath()));
    if (!tmpl || !tmpl->adapter) {
        std::fprintf(stderr, "[viewer] Save As: no source model to write\n");
        return false;
    }
    try {
        whiteout::mdx::Writer writer;
        writer.write(outPath, tmpl->adapter->SourceModel(), dialect);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[viewer] Save As FAILED '%s': %s\n", outPath.c_str(), e.what());
        return false;
    }
    std::printf("[viewer] Saved model: %s\n", outPath.c_str());
    return true;
}

} // namespace

void ViewerUI::SaveAsDialog() {
    NFD::UniquePathU8 outPath;
    // Two separate filter entries (not "mdx,mdl") so NFD appends the right
    // extension for whichever the user selects — that extension is then how
    // we decide binary vs text.
    nfdu8filteritem_t filter[2] = {{"MDX model (binary)", "mdx"},
                                   {"MDL model (text)", "mdl"}};
    if (NFD::SaveDialog(outPath, filter, 2) != NFD_OKAY)
        return;

    std::string path = outPath.get();
    std::string ext = std::filesystem::path(path).extension().string();
    for (auto& c : ext)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    if (ext == ".mdl") {
        // MDL needs a dialect — stash the path and pop the modal next frame.
        pendingSaveMdlPath_ = std::move(path);
        openDialectPopup_ = true;
    } else {
        // MDX (binary) — dialect is irrelevant to the writer here.
        WriteCurrentModel(app_, path, whiteout::mdx::MdlFormat::WarcraftIII);
    }
}

void ViewerUI::BuildSaveAsPopup() {
    if (openDialectPopup_) {
        ImGui::OpenPopup("Save As MDL");
        openDialectPopup_ = false;
    }

    const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal("Save As MDL", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        return;

    ImGui::TextUnformatted("Which MDL dialect should be written?");
    ImGui::Spacing();
    ImGui::TextDisabled("Warcraft III  - engine-faithful, loads in-game.");
    ImGui::TextDisabled("Hiveworkshop  - community tools (Retera, Magos, MdlVis).");
    ImGui::Separator();

    if (ImGui::Button("Warcraft III", ImVec2(130, 0))) {
        WriteCurrentModel(app_, pendingSaveMdlPath_, whiteout::mdx::MdlFormat::WarcraftIII);
        pendingSaveMdlPath_.clear();
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Hiveworkshop", ImVec2(130, 0))) {
        WriteCurrentModel(app_, pendingSaveMdlPath_, whiteout::mdx::MdlFormat::Hiveworkshop);
        pendingSaveMdlPath_.clear();
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(80, 0))) {
        pendingSaveMdlPath_.clear();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void ViewerUI::BuildExportPopup() {
    if (openExportPopup_) {
        ImGui::OpenPopup("Export Animation Frames");
        openExportPopup_ = false;
    }

    const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal("Export Animation Frames", nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize))
        return;

    const auto& seqs = app_.SequenceNames();
    if (seqs.empty()) {
        ImGui::TextUnformatted("The loaded model has no animations.");
        if (ImGui::Button("Close", ImVec2(80, 0)))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }

    exportSeqIdx_ = std::clamp(exportSeqIdx_, 0, static_cast<i32>(seqs.size()) - 1);

    // ---- Animation ----
    ImGui::SetNextItemWidth(280);
    if (ImGui::BeginCombo("Animation", seqs[exportSeqIdx_].c_str())) {
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
    ImGui::InputInt("FPS", &exportFps_);
    exportFps_ = std::clamp(exportFps_, 1, 240);

    // ---- Format ----
    {
        const char* formats[] = {"PNG frames", "Animated GIF"};
        ImGui::SetNextItemWidth(200);
        ImGui::Combo("Format", &exportFormat_, formats, 2);
    }

    // ---- Output folder ----
    {
        char tmp[1024];
        std::snprintf(tmp, sizeof(tmp), "%s", exportFolder_.c_str());
        ImGui::SetNextItemWidth(360);
        if (ImGui::InputText("##exportfolder", tmp, sizeof(tmp)))
            exportFolder_ = tmp;
        ImGui::SameLine();
        if (ImGui::Button("Browse...##export")) {
            NFD::UniquePathU8 outPath;
            if (NFD::PickFolder(outPath) == NFD_OKAY)
                exportFolder_ = outPath.get();
        }
        ImGui::SameLine();
        ImGui::TextUnformatted("Output Folder");
    }

    // ---- Duration / frame-count preview ----
    const bool gif = (exportFormat_ == 1);
    const auto& ranges = app_.SequenceRanges();
    if (exportSeqIdx_ < static_cast<i32>(ranges.size())) {
        const SequenceInfo& s = ranges[exportSeqIdx_];
        const i32 durMs = std::max(0, s.endMs - s.startMs);
        const i32 frames = std::max(
            1, static_cast<i32>(std::llround(static_cast<f64>(durMs) * exportFps_ / 1000.0)));
        ImGui::TextDisabled("%d ms - %d frame(s) at %d FPS", durMs, frames, exportFps_);
    }
    ImGui::TextDisabled(gif ? "Output: <model>_<animation>.gif"
                            : "Output: <model>_<animation>_<id>.png");

    ImGui::Separator();

    const bool canExport = !exportFolder_.empty();
    ImGui::BeginDisabled(!canExport);
    if (ImGui::Button("Export", ImVec2(120, 0))) {
        app_.RequestAnimationExport(
            exportSeqIdx_, exportFps_,
            gif ? ExportFormat::Gif : ExportFormat::PngFrames,
            io::FsPathFromUtf8(exportFolder_));
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(80, 0)))
        ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

void ViewerUI::BuildMenuBar() {
    RenderService& svc = app_.Service();
    DisplayFlags df = svc.Settings().GetDisplayFlags();
    bool dfChanged = false;

    if (ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("Open Model...", "Ctrl+O"))
                OpenFileDialog();
            const bool hasModel = !app_.CurrentModelPath().empty();
            if (ImGui::MenuItem("Save As...", "Ctrl+Shift+S", false, hasModel))
                SaveAsDialog();
            const bool hasAnims = hasModel && !app_.SequenceNames().empty();
            if (ImGui::MenuItem("Export Animation Frames...", nullptr, false, hasAnims)) {
                model::Actor* focus = app_.FocusActorPtr();
                exportSeqIdx_ = focus ? focus->animation.ActiveSequenceIndex() : 0;
                openExportPopup_ = true;
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Exit"))
                glfwSetWindowShouldClose(app_.Window(), GLFW_TRUE);
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("View")) {
            dfChanged |= ImGui::MenuItem("Grid", nullptr, &df.showGrid);
            dfChanged |= ImGui::MenuItem("Particles", nullptr, &df.showParticles);
            dfChanged |= ImGui::MenuItem("Ribbons", nullptr, &df.showRibbons);
            dfChanged |= ImGui::MenuItem("Event Objects", nullptr, &df.showEvents);
            ImGui::MenuItem("View Cube", nullptr, &showViewCube_);

            ImGui::Separator();
            if (ImGui::BeginMenu("Tileset")) {
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

        if (ImGui::BeginMenu("Debug")) {
            dfChanged |= ImGui::MenuItem("Collision Markers", nullptr, &df.showCollisions);
            dfChanged |= ImGui::MenuItem("Light Markers", nullptr, &df.showLights);
            ImGui::Separator();

            if (ImGui::BeginMenu("Debug View")) {
                const i32 cur = svc.Settings().HdDebugMode();
                for (i32 i = 0; i < static_cast<i32>(kDebugVisLabels.size()); ++i) {
                    if (ImGui::MenuItem(kDebugVisLabels[i], nullptr, i == cur)) {
                        svc.Settings().SetHdDebugMode(i);
                        SaveIni(app_);
                    }
                }
                ImGui::EndMenu();
            }

            if (ImGui::BeginMenu("LOD")) {
                const i32 cur = svc.Settings().LodOverride();
                const i32 curIdx = (cur < 0) ? 0 : (1 + std::clamp(cur, 0, 3));
                for (i32 i = 0; i < static_cast<i32>(kLodLabels.size()); ++i) {
                    if (ImGui::MenuItem(kLodLabels[i], nullptr, i == curIdx)) {
                        svc.Settings().SetLodOverride(i == 0 ? -1 : (i - 1));
                        SaveIni(app_);
                    }
                }
                ImGui::EndMenu();
            }

            ImGui::EndMenu();
        }

        if (ImGui::MenuItem("Settings"))
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
        if (ImGui::BeginCombo("Animation", seqs[std::clamp(sel, 0, (i32)seqs.size() - 1)].c_str())) {
            for (i32 i = 0; i < static_cast<i32>(seqs.size()); ++i) {
                const bool isSel = (i == sel);
                if (ImGui::Selectable(seqs[i].c_str(), isSel)) {
                    if (focus) {
                        const i32 prev = focus->animation.ActiveSequenceIndex();
                        focus->animation.SetActiveSequenceIndex(i);
                        if (i != prev) {
                            const std::string& name = seqs[i];
                            const bool keep =
                                (name.find("decay") != std::string::npos) ||
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
                                  ? "Free Camera"
                                  : presetNames[active].c_str();
        ImGui::SetNextItemWidth(140);
        if (ImGui::BeginCombo("Camera", preview)) {
            if (ImGui::Selectable("Free Camera", active < 0))
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
        ImGui::TextUnformatted("Team:");
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
        if (ImGui::Combo("Lighting", &sel, kLightingLabels.data(),
                         static_cast<i32>(kLightingLabels.size()))) {
            svc.Settings().SetLightingMode(static_cast<LightingMode>(sel));
            SaveIni(app_);
        }
    }

    ImGui::End();
    ImGui::PopStyleVar(2);
}

void ViewerUI::BuildSettingsWindow() {
    RenderService& svc = app_.Service();
    ImGui::SetNextWindowSize(ImVec2(440, 540), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Settings", &settingsOpen_)) {
        ImGui::End();
        return;
    }

    if (!ImGui::BeginTabBar("##SettingsTabs")) {
        ImGui::End();
        return;
    }

    if (ImGui::BeginTabItem("General")) {
    // ---- Background colour ----
    {
        const u32 bg = svc.Settings().BackgroundColorRaw();
        f32 col[3] = {
            static_cast<f32>(bg & 0xFFu) / 255.0f,
            static_cast<f32>((bg >> 8) & 0xFFu) / 255.0f,
            static_cast<f32>((bg >> 16) & 0xFFu) / 255.0f,
        };
        if (ImGui::ColorEdit3("Background", col)) {
            svc.Settings().SetBackgroundColor(static_cast<u8>(col[0] * 255.0f),
                                              static_cast<u8>(col[1] * 255.0f),
                                              static_cast<u8>(col[2] * 255.0f));
            SaveIni(app_);
        }
    }

    // ---- Exposure ----
    {
        f32 exposure = svc.Settings().GetTonemapExposure();
        if (ImGui::SliderFloat("Exposure", &exposure, 0.0f, 3.0f, "%.2f")) {
            svc.Settings().SetTonemapExposure(exposure);
            SaveIni(app_);
        }
    }

    // ---- Sound volume ----
    {
        f32 vol = svc.Sound().GetVolume();
        if (ImGui::SliderFloat("SND Volume", &vol, 0.0f, 1.0f, "%.2f")) {
            svc.Sound().SetVolume(vol);
            SaveIni(app_);
        }
    }

    // ---- Loop non-looping ----
    {
        bool on = app_.LoopNonLoopingPolicy();
        if (ImGui::Checkbox("Loop NonLooping animations", &on)) {
            app_.SetLoopNonLoopingPolicy(on);
            SaveIni(app_);
        }
    }

    ImGui::Separator();

    // ---- Time of day ----
    if (auto* dnc = svc.GetDncService()) {
        const f32 hpd = dnc->GetHoursPerDay();
        f32 tod = dnc->GetTimeOfDay();
        if (ImGui::SliderFloat("Time of Day", &tod, 0.0f, hpd, "%.2f h")) {
            dnc->SetTimeOfDay(tod);
            SaveIni(app_);
        }
        bool animating = dnc->GetTodScale() > 0.0f;
        if (ImGui::Checkbox("Animate TOD", &animating)) {
            dnc->SetTodScale(animating ? 1.0f : 0.0f);
            SaveIni(app_);
        }
    }

    ImGui::Separator();

    // ---- IBL mode ----
    {
        i32 sel = static_cast<i32>(svc.Settings().GetIblMode());
        if (ImGui::Combo("IBL", &sel, kIblLabels.data(),
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
        if (ImGui::Combo("Shadows", &sel, kShadowLabels.data(),
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

    ImGui::Separator();

    // ---- DNC model path ----
    // ImGui InputText needs a writable buffer the UI owns across frames. We
    // mirror DncService.UnitMdlPath() into dncPathBuf_ whenever the user
    // isn't actively typing (matches the old EN_KILLFOCUS commit-on-blur
    // flow); when they deactivate the field after an edit we push the new
    // value back into the service.
    if (auto* dnc = svc.GetDncService()) {
        char buf[512];
        std::snprintf(buf, sizeof(buf), "%s",
                      dncPathBuf_.empty() ? dnc->UnitMdlPath().c_str() : dncPathBuf_.c_str());
        if (ImGui::InputText("DNC Model", buf, sizeof(buf)))
            dncPathBuf_ = buf;
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            dnc->SetUnitMdl(dncPathBuf_);
            SaveIni(app_);
        }
        ImGui::SameLine();
        if (ImGui::Button("Reset##dnc")) {
            dncPathBuf_ = dnc::DncService::kDefaultUnitMdl;
            dnc->SetUnitMdl(dncPathBuf_);
            SaveIni(app_);
        }
    }

    ImGui::Separator();
    ImGui::TextDisabled("Startup settings (take effect on next launch)");

    // ---- Default backend ----
    // On Linux only Vulkan is built (D3D11/D3D12 are gated to WIN32 in
    // CMake + gfx_factory), so the combo collapses to a disabled
    // single-entry indicator instead of offering picks that would error.
#if defined(_WIN32)
    {
        i32 sel = BackendToIdx(svc.Settings().DefaultBackend());
        if (ImGui::Combo("Backend", &sel, kBackendLabels.data(),
                         static_cast<i32>(kBackendLabels.size()))) {
            svc.Settings().SetDefaultBackend(IdxToBackend(sel));
            SaveIni(app_);
        }
    }
#else
    {
        ImGui::BeginDisabled();
        i32 sel = 0;
        const char* vkOnly[] = {"Vulkan"};
        ImGui::Combo("Backend", &sel, vkOnly, 1);
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
        const char* preview = cur.empty() ? "(Auto - highest VRAM)" : cur.c_str();
        if (ImGui::BeginCombo("Device", preview)) {
            if (ImGui::Selectable("(Auto - highest VRAM)", cur.empty())) {
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
        if (ImGui::Checkbox("Graphics Debug (validation)", &on)) {
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
    if (ImGui::BeginTabItem("IO")) {
        auto& provider = svc.Scene().GetContentProvider();
        if (!ioBufsInitialised_) {
            installPathBuf_ = provider.InstallPath();
            ioBufsInitialised_ = true;
        }

        const std::string& autoDetected = provider.Wc3Path();
        if (autoDetected.empty())
            ImGui::TextDisabled("Warcraft III install not auto-detected.");
        else
            ImGui::TextDisabled("Auto-detected: %s", autoDetected.c_str());
        ImGui::Spacing();

        // Commit the entire IO state (install path + flags + list) to ini.
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
            if (ImGui::Button("Browse...##install")) {
                NFD::UniquePathU8 outPath;
                if (NFD::PickFolder(outPath) == NFD_OKAY) {
                    installPathBuf_ = outPath.get();
                    provider.SetInstallPath(installPathBuf_);
                    saveIo();
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Reset##install")) {
                provider.SetInstallPath("");
                installPathBuf_ = provider.InstallPath();
                saveIo();
            }
            ImGui::SameLine();
            ImGui::TextUnformatted("Install Path");
        }

        ImGui::Spacing();
        ImGui::Separator();

        // ---- Ignore flags ----
        {
            bool ignoreCasc = provider.IgnoreCasc();
            if (ImGui::Checkbox("Ignore CASC", &ignoreCasc)) {
                provider.SetIgnoreCasc(ignoreCasc);
                saveIo();
            }
            bool ignoreMpq = provider.IgnoreMpq();
            if (ImGui::Checkbox("Ignore MPQ", &ignoreMpq)) {
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
        ImGui::TextUnformatted("MPQs (load order, first wins)");
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
            if (ImGui::Button("Add MPQ")) {
                mpqs.push_back(newMpqEntryBuf_);
                newMpqEntryBuf_.clear();
                mpqsDirty = true;
            }
            ImGui::EndDisabled();
        }

        if (ImGui::SmallButton("Reset to defaults")) {
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
        ImGui::TextDisabled("CASC: %s", provider.HasCasc() ? "open" : "not loaded");
        ImGui::TextDisabled("MPQ:  %s open", provider.HasMpq() ? "yes" : "no");

        ImGui::EndTabItem();
    }

    ImGui::EndTabBar();
    ImGui::End();
}

} // namespace whiteout::flakes
