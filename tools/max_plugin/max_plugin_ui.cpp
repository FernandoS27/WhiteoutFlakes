#include "max_plugin_ui.h"

#include "render_window.h"
#include "renderer/assets/replaceable_texture_manager.h"
#include "renderer/camera.h"
#include "renderer/debug/debug_renderer.h"
#include "renderer/dnc/dnc_service.h"
#include "renderer/model/model_instance.h"
#include "renderer/particle/splat_service.h"
#include "renderer/render_service.h"
#include "renderer/scene_manager.h"
#include "renderer/shadow/shadow_service.h"
#include "settings_ini.h"
#include "whiteout/flakes/display.h"
#include "whiteout/flakes/enums.h"
#include "whiteout/flakes/sound_emitter.h"
#include "whiteout/flakes/util/path_utf8.h"
#include "whiteout/flakes/util/replaceable_paths.h"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

namespace whiteout::flakes {

namespace {

// Persists settings to WhiteoutFlakes.ini after every UI change. The Max
// plugin doesn't track the loop-non-looping policy (Max drives the
// timeline), so we always pass `true` — settings_ini's writer just stores
// whatever it's given.
void SaveIni(RenderWindow& win) {
    SaveSettingsIni(win.Service(), true);
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

MaxPluginUI::MaxPluginUI(RenderWindow& win) : win_(win) {}

void MaxPluginUI::BuildFrame() {
    BuildMenuBar();
    BuildToolbar();
    BuildViewCubeWidget();
    if (settingsOpen_)
        BuildSettingsWindow();
}

void MaxPluginUI::BuildViewCubeWidget() {
    auto& dbg = win_.Service().Debug();
    const auto r = dbg.GetViewCubeRect();
    const f32 x = static_cast<f32>(r.left);
    const f32 y = static_cast<f32>(r.top);
    const f32 w = static_cast<f32>(r.right - r.left);
    const f32 h = static_cast<f32>(r.bottom - r.top);
    if (w <= 0.0f || h <= 0.0f)
        return;

    ImGui::SetNextWindowPos(ImVec2(x, y));
    ImGui::SetNextWindowSize(ImVec2(w, h));
    constexpr ImGuiWindowFlags kFlags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoFocusOnAppearing;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    if (ImGui::Begin("##viewcube", nullptr, kFlags)) {
        ImGui::InvisibleButton("##viewcube_hit", ImVec2(w, h));
        const bool hovered = ImGui::IsItemHovered();
        dbg.SetViewCubeHovered(hovered);
        if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
            const ImVec2 mp = ImGui::GetIO().MousePos;
            const i32 hit =
                dbg.HitTestViewCube(static_cast<i32>(mp.x), static_cast<i32>(mp.y));
            if (hit == 6)
                win_.Service().Scene().Camera().Reset();
            else if (hit >= 0)
                win_.Service().Scene().Camera().SnapToViewCubeFace(hit);
        }
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
}

void MaxPluginUI::BuildMenuBar() {
    RenderService& svc = win_.Service();
    DisplayFlags df = svc.Settings().GetDisplayFlags();
    bool dfChanged = false;

    if (ImGui::BeginMainMenuBar()) {
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
                        SaveIni(win_);
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
                        SaveIni(win_);
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
                        SaveIni(win_);
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
        SaveIni(win_);
    }
}

void MaxPluginUI::BuildToolbar() {
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

    RenderService& svc = win_.Service();
    model::Actor* focus = svc.Scene().Actors().Find(win_.FocusActor());

    // ---- Animation sequence (driven by Max's timeline; the dropdown only
    //      picks which range we're previewing) ----
    const auto seqs = win_.SequenceNamesSnapshot();
    if (!seqs.empty()) {
        i32 sel = focus ? focus->animation.ActiveSequenceIndex() : 0;
        sel = std::clamp(sel, 0, (i32)seqs.size() - 1);
        ImGui::SetNextItemWidth(220);
        if (ImGui::BeginCombo("Animation", seqs[sel].c_str())) {
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
    const auto presets = win_.CameraPresetsSnapshot();
    if (!presets.empty()) {
        const i32 active = win_.ActiveCameraPresetIdx();
        const char* preview = (active < 0 || active >= (i32)presets.size())
                                  ? "Free Camera"
                                  : presets[active].name.c_str();
        ImGui::SetNextItemWidth(140);
        if (ImGui::BeginCombo("Camera", preview)) {
            if (ImGui::Selectable("Free Camera", active < 0))
                win_.ActivateCameraPreset(-1);
            for (i32 i = 0; i < (i32)presets.size(); ++i) {
                const bool isSel = (i == active);
                if (ImGui::Selectable(presets[i].name.c_str(), isSel))
                    win_.ActivateCameraPreset(i);
                if (isSel)
                    ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
    }

    // ---- Team colour ----
    {
        u32 tcRaw = focus ? (focus->teamColor & 0x00FFFFFFu) : 0x000000FFu;
        f32 col[3] = {
            static_cast<f32>(tcRaw & 0xFFu) / 255.0f,
            static_cast<f32>((tcRaw >> 8) & 0xFFu) / 255.0f,
            static_cast<f32>((tcRaw >> 16) & 0xFFu) / 255.0f,
        };
        ImGuiColorEditFlags flags = ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel;
        ImGui::TextUnformatted("Team:");
        ImGui::SameLine();
        if (ImGui::ColorEdit3("##team", col, flags) && focus) {
            focus->SetTeamColor(static_cast<u8>(col[0] * 255.0f),
                                static_cast<u8>(col[1] * 255.0f),
                                static_cast<u8>(col[2] * 255.0f));
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
            SaveIni(win_);
        }
    }

    ImGui::End();
    ImGui::PopStyleVar(2);
}

void MaxPluginUI::BuildSettingsWindow() {
    RenderService& svc = win_.Service();
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
            SaveIni(win_);
        }
    }

    // ---- Exposure ----
    {
        f32 exposure = svc.Settings().GetTonemapExposure();
        if (ImGui::SliderFloat("Exposure", &exposure, 0.0f, 3.0f, "%.2f")) {
            svc.Settings().SetTonemapExposure(exposure);
            SaveIni(win_);
        }
    }

    // ---- Sound volume ----
    {
        f32 vol = svc.Sound().GetVolume();
        if (ImGui::SliderFloat("SND Volume", &vol, 0.0f, 1.0f, "%.2f")) {
            svc.Sound().SetVolume(vol);
            SaveIni(win_);
        }
    }

    ImGui::Separator();

    // ---- Time of day ----
    if (auto* dnc = svc.GetDncService()) {
        const f32 hpd = dnc->GetHoursPerDay();
        f32 tod = dnc->GetTimeOfDay();
        if (ImGui::SliderFloat("Time of Day", &tod, 0.0f, hpd, "%.2f h")) {
            dnc->SetTimeOfDay(tod);
            SaveIni(win_);
        }
        bool animating = dnc->GetTodScale() > 0.0f;
        if (ImGui::Checkbox("Animate TOD", &animating)) {
            dnc->SetTodScale(animating ? 1.0f : 0.0f);
            SaveIni(win_);
        }
    }

    ImGui::Separator();

    // ---- IBL mode ----
    {
        i32 sel = static_cast<i32>(svc.Settings().GetIblMode());
        if (ImGui::Combo("IBL", &sel, kIblLabels.data(),
                         static_cast<i32>(kIblLabels.size()))) {
            svc.Settings().SetIblMode(static_cast<IblMode>(sel));
            SaveIni(win_);
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
                SaveIni(win_);
            }
        }
    }

    ImGui::Separator();

    // ---- DNC model path ----
    if (auto* dnc = svc.GetDncService()) {
        char buf[512];
        std::snprintf(buf, sizeof(buf), "%s",
                      dncPathBuf_.empty() ? dnc->UnitMdlPath().c_str() : dncPathBuf_.c_str());
        if (ImGui::InputText("DNC Model", buf, sizeof(buf)))
            dncPathBuf_ = buf;
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            dnc->SetUnitMdl(dncPathBuf_);
            SaveIni(win_);
        }
        ImGui::SameLine();
        if (ImGui::Button("Reset##dnc")) {
            dncPathBuf_ = dnc::DncService::kDefaultUnitMdl;
            dnc->SetUnitMdl(dncPathBuf_);
            SaveIni(win_);
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
            SaveIni(win_);
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
        const char* preview = cur.empty() ? "(Auto - highest VRAM)" : cur.c_str();
        if (ImGui::BeginCombo("Device", preview)) {
            if (ImGui::Selectable("(Auto - highest VRAM)", cur.empty())) {
                svc.Settings().SetPreferredDevice("");
                SaveIni(win_);
            }
            for (const auto& n : devices) {
                const bool isSel = (n == cur);
                if (ImGui::Selectable(n.c_str(), isSel)) {
                    svc.Settings().SetPreferredDevice(n);
                    SaveIni(win_);
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
            SaveIni(win_);
        }
    }

    ImGui::End();
}

} // namespace whiteout::flakes
