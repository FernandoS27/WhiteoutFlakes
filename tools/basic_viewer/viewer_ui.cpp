#include "viewer_ui.h"

#include "renderer/assets/replaceable_texture_manager.h"
#include "renderer/dnc/dnc_service.h"
#include "renderer/model/model_instance.h"
#include "renderer/particle/splat_service.h"
#include "renderer/render_service.h"
#include "renderer/scene_manager.h"
#include "renderer/shadow/shadow_service.h"
#include "settings_ini.h"
#include "viewer_app.h"
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
constexpr std::array<const char*, 3> kBackendLabels = {"D3D11", "D3D12", "Vulkan"};

i32 BackendToIdx(gfx::GfxApi b) {
    switch (b) {
    case gfx::GfxApi::D3D11:
        return 0;
    case gfx::GfxApi::D3D12:
        return 1;
    case gfx::GfxApi::Vulkan:
        return 2;
    }
    return 1;
}
gfx::GfxApi IdxToBackend(i32 idx) {
    switch (idx) {
    case 0:
        return gfx::GfxApi::D3D11;
    case 2:
        return gfx::GfxApi::Vulkan;
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
    if (settingsOpen_)
        BuildSettingsWindow();
    if (showDemo_)
        ImGui::ShowDemoWindow(&showDemo_);
}

void ViewerUI::OpenFileDialog() {
    // Use the UTF-8 NFD entry points so the filter strings stay as plain
    // `char` literals on every platform — the native variant takes wchar_t
    // on Windows, which would break these inline string constants.
    NFD::UniquePathU8 outPath;
    nfdu8filteritem_t filter[1] = {{"MDX Model", "mdx"}};
    if (NFD::OpenDialog(outPath, filter, 1) == NFD_OKAY) {
        std::filesystem::path p = io::FsPathFromUtf8(outPath.get());
        app_.LoadModel(p);
    }
}

void ViewerUI::BuildMenuBar() {
    RenderService& svc = app_.Service();
    DisplayFlags df = svc.Settings().GetDisplayFlags();
    bool dfChanged = false;

    if (ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("Open MDX...", "Ctrl+O"))
                OpenFileDialog();
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
                    }
                }
                ImGui::EndMenu();
            }

            ImGui::Separator();
            ImGui::MenuItem("ImGui Demo", nullptr, &showDemo_);
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
    {
        i32 sel = BackendToIdx(svc.Settings().DefaultBackend());
        if (ImGui::Combo("Backend", &sel, kBackendLabels.data(),
                         static_cast<i32>(kBackendLabels.size()))) {
            svc.Settings().SetDefaultBackend(IdxToBackend(sel));
            SaveIni(app_);
        }
    }

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
        const char* preview = cur.empty() ? "(Auto — highest VRAM)" : cur.c_str();
        if (ImGui::BeginCombo("Device", preview)) {
            if (ImGui::Selectable("(Auto — highest VRAM)", cur.empty())) {
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

    ImGui::End();
}

} // namespace whiteout::flakes
