#include "ui/toolbar.h"

#include "app/viewer_app.h"
#include "color_pack.h"
#include "localization.h"
#include "progress_dialog.h"
#include "renderer/model/model_instance.h"
#include "renderer/render_service.h"
#include "ui/animation_window.h"
#include "ui/ui_context.h"
#include "ui/ui_metrics.h"
#include "ui/widgets.h"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>

namespace whiteout::flakes {

namespace {

constexpr std::array<const char*, 3> kLightingKeys = {"lighting.ingame", "lighting.glue", "lighting.dynamic"};

} // namespace

Toolbar::Toolbar(UiContext& ctx, AnimationWindow& animationWindow)
    : ctx_(ctx), animationWindow_(animationWindow) {}

bool Toolbar::ShotHolds(const char* popupId) const {
    return shotPopupId_ && std::strcmp(shotPopupId_, popupId) == 0;
}

void Toolbar::Build() {
    // Anchored just below the main menu bar, the viewport's width, fixed height,
    // no decorations.
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const f32 menuH = ImGui::GetFrameHeight();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x, vp->WorkPos.y));
    ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x, menuH + ui::kStripPadding));
    ImGuiWindowFlags wf = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                          ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar |
                          ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    if (!ImGui::Begin("##toolbar", nullptr, wf)) {
        ImGui::End();
        ImGui::PopStyleVar(2);
        return;
    }

    BuildTransport();
    BuildSequencePicker();

    // ---- Animation window (StarCraft II only) ----
    // Beside the sequence dropdown because it is the rest of that control: an
    // `.m3` needs several plays at once plus the global loops it starts itself.
    if (const Sc2AnimationFiles* sc2 = ctx_.app.Features().Sc2(); sc2 && sc2->CanAttach()) {
        if (ui::IconButton("##animfiles", ui::ToolbarIcon::Tracks, "toolbar.animfiles", "toolbar.animfiles.tip",
                           animationWindow_.IsOpen()))
            animationWindow_.SetOpen(!animationWindow_.IsOpen());
        ImGui::SameLine();
    }

    BuildCameraPicker();
    BuildTeamColor();
    if (WowAppearance* wow = ctx_.app.Features().Wow())
        BuildWowControls(*wow);
    BuildD3Equip();
    BuildLightingMode();

    // Background work nobody asked for — the client-database prewarm — reports
    // here instead of taking the screen. Nothing when idle, or when the running
    // task is one the modal is already showing.
    tools::DrawProgressStatus(ctx_.app.Tasks());

    ImGui::End();
    ImGui::PopStyleVar(2);
}

// First on the row and never hidden. The scene clock is what every profile
// advances on — animation, particles, ribbons and corn-fx alike — so one pair of
// buttons covers every format, including one with no sequence list at all.
void Toolbar::BuildTransport() {
    PlaybackController& playback = ctx_.app.Playback();
    const bool paused = playback.IsPaused();
    if (ui::IconButton("##transport.toggle", paused ? ui::ToolbarIcon::Play : ui::ToolbarIcon::Pause,
                       paused ? "toolbar.play" : "toolbar.pause", paused ? "toolbar.play.tip" : "toolbar.pause.tip"))
        playback.SetPaused(!paused);
    ImGui::SameLine();
    if (ui::IconButton("##transport.restart", ui::ToolbarIcon::Restart, "toolbar.restart", "toolbar.restart.tip"))
        playback.RestartPlayback();
    ImGui::SameLine();
}

void Toolbar::BuildSequencePicker() {
    PlaybackController& playback = ctx_.app.Playback();
    const auto& seqs = playback.SequenceNames();
    if (seqs.empty())
        return;
    const i32 sel = playback.ActiveSequence();
    ui::ToolbarLabel("toolbar.animation");
    ImGui::SetNextItemWidth(ui::kSequenceComboWidth);
    if (ImGui::BeginCombo("##animation", seqs[std::clamp(sel, 0, static_cast<i32>(seqs.size()) - 1)].c_str())) {
        for (i32 i = 0; i < static_cast<i32>(seqs.size()); ++i) {
            const bool isSel = (i == sel);
            if (ImGui::Selectable(seqs[i].c_str(), isSel))
                playback.SelectSequence(i);
            if (isSel)
                ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
}

void Toolbar::BuildCameraPicker() {
    PlaybackController& playback = ctx_.app.Playback();
    const auto& presets = playback.CameraPresets();
    const i32 active = playback.ActiveCameraPreset().value_or(-1);
    const char* preview = (active < 0 || active >= static_cast<i32>(presets.size()))
                              ? i18n::tr("toolbar.camera.free")
                              : presets[static_cast<usize>(active)].name.c_str();
    ui::ToolbarLabel("toolbar.camera");
    ImGui::SetNextItemWidth(ui::kCameraComboWidth);
    if (ImGui::BeginCombo("##camera", preview)) {
        if (ImGui::Selectable(i18n::tr("toolbar.camera.free"), active < 0))
            playback.ActivateCameraPreset(std::nullopt);
        for (i32 i = 0; i < static_cast<i32>(presets.size()); ++i) {
            const bool isSel = (i == active);
            if (ImGui::Selectable(presets[static_cast<usize>(i)].name.c_str(), isSel))
                playback.ActivateCameraPreset(i);
            if (isSel)
                ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
}

void Toolbar::BuildTeamColor() {
    renderer::model::Actor* focus = ctx_.app.Playback().FocusActor();
    // Red when nothing is loaded, which is what a fresh actor wears.
    tools::Rgb8 team = tools::UnpackBgr(focus ? focus->teamColor : 0x000000FFu);
    ui::ToolbarLabel("toolbar.team");
    if (ui::ColorEditRgb8("##team", team, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel) && focus)
        focus->SetTeamColor(team.r, team.g, team.b);
    ImGui::SameLine();
}

void Toolbar::BuildWowControls(WowAppearance& wow) {
    // ---- Creature skin ----
    // A creature model leaves its skin blank for the game to fill; this is which
    // fill. Absent for every other model, which is most of them.
    if (const auto skins = wow.SkinNames(); !skins.empty()) {
        const u32 sel = wow.Skin() % static_cast<u32>(skins.size());
        ui::ToolbarLabel("toolbar.skin");
        ImGui::SetNextItemWidth(ui::kSkinComboWidth);
        if (ImGui::BeginCombo("##skin", skins[sel].c_str())) {
            for (u32 i = 0; i < static_cast<u32>(skins.size()); ++i) {
                const bool isSel = (i == sel);
                if (ImGui::Selectable(skins[i].c_str(), isSel))
                    wow.SetSkin(i);
                if (isSel)
                    ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
    }

    // ---- Character customisation ----
    // A dozen options behind one button rather than a dozen combos: a character
    // offers skin, face, hair, beard, eyes and more, and the toolbar has room for
    // none of that.
    if (const auto options = wow.CharacterOptions(); !options.empty()) {
        if (ImGui::Button(i18n::tr("toolbar.customize")))
            ImGui::OpenPopup("##customize");
        if (ShotHolds("##customize"))
            ImGui::OpenPopup("##customize", ImGuiPopupFlags_NoReopen);
        if (ImGui::BeginPopup("##customize")) {
            for (const auto& opt : options) {
                if (opt.choiceCount == 0)
                    continue;
                char label[128];
                std::snprintf(label, sizeof(label), "%s##opt%u", opt.name.c_str(), opt.optionId);
                i32 sel = static_cast<i32>(opt.selected);
                ImGui::SetNextItemWidth(ui::kCustomizeSliderWidth);
                // Mostly unnamed — a skin swatch has a colour, not a name — so
                // the choices are numbered rather than labelled.
                if (ImGui::SliderInt(label, &sel, 0, static_cast<i32>(opt.choiceCount) - 1))
                    wow.SetCharacterChoice(opt.optionId, static_cast<u32>(sel));
            }
            ImGui::EndPopup();
        }
        ImGui::SameLine();
    }
}

// The Diablo III half of the same idea, and the opposite problem: a `.m2`
// character leaves its geosets blank for the game to fill, while a `.app` ships
// every armour variant at once for the game to pick between. Absent for every
// model that is not a player character.
void Toolbar::BuildD3Equip() {
    D3Wardrobe* d3 = ctx_.app.Features().D3();
    if (!d3)
        return;
    const auto slots = d3->CharacterSlots();
    if (slots.empty())
        return;
    // The registry build starts as soon as a character is on screen, so it runs
    // while the user is still looking at the model and the popup usually opens
    // ready instead of onto its progress bar.
    d3->EnsureItemRegistry();
    if (ImGui::Button(i18n::tr("toolbar.equip")))
        ImGui::OpenPopup("##d3equip");
    if (ShotHolds("##d3equip"))
        ImGui::OpenPopup("##d3equip", ImGuiPopupFlags_NoReopen);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", i18n::tr("toolbar.equip.tip"));
    if (ImGui::BeginPopup("##d3equip")) {
        d3Equip_.Build(*d3, slots);
        ImGui::EndPopup();
    }
    ImGui::SameLine();
}

void Toolbar::BuildLightingMode() {
    auto& settings = ctx_.app.Service().Settings();
    i32 sel = static_cast<i32>(settings.GetLightingMode());
    ui::ToolbarLabel("toolbar.lighting");
    ImGui::SetNextItemWidth(ui::kLightingComboWidth);
    if (ui::KeyCombo("##lighting", sel, kLightingKeys)) {
        settings.SetLightingMode(static_cast<LightingMode>(sel));
        ctx_.MarkSettingsDirty();
    }
}

} // namespace whiteout::flakes
