#include "ui/ribbon.h"

#include "app/viewer_app.h"
#include "capture/export_window.h"
#include "color_pack.h"
#include "imgui_app_icon.h"
#include "imgui_ribbon.h"
#include "localization.h"
#include "progress_dialog.h"
#include "renderer/model/model_instance.h"
#include "renderer/render_service.h"
#include "ui/animation_window.h"
#include "ui/export_dialogs.h"
#include "ui/menu_bar.h"
#include "ui/open_dialog.h"
#include "ui/settings_window.h"
#include "ui/ui_context.h"
#include "ui/ui_metrics.h"
#include "ui/widgets.h"
#include "whiteout/flakes/util/path_utf8.h"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>

namespace whiteout::flakes {

namespace {

constexpr std::array<const char*, 3> kLightingKeys = {"lighting.ingame", "lighting.glue", "lighting.dynamic"};

/// The rail's tabs, in order. One today; a second game-facing mode is a row
/// here and a branch in Build.
struct ModeTab {
    ViewerMode mode;
    const char* id;
    ui::Icon icon;
    const char* key;
};
constexpr std::array<ModeTab, 1> kModes = {{
    {ViewerMode::Preview, "##mode.preview", ui::Icon::Cube, "ribbon.mode.preview"},
}};

/// A ribbon button says what it does in two words; the tooltip is where the
/// sentence goes.
void Tip(const char* key) {
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", i18n::tr(key));
}

} // namespace

Ribbon::Ribbon(UiContext& ctx, MenuBar& menus, AnimationWindow& animationWindow, OpenDialog& openDialog,
               ExportDialogs& exportDialogs, ExportWindow& exportWindow, SettingsWindow& settings)
    : ctx_(ctx), menus_(menus), animationWindow_(animationWindow), openDialog_(openDialog),
      exportDialogs_(exportDialogs), exportWindow_(exportWindow), settings_(settings) {}

bool Ribbon::ShotHolds(const char* popupId) const {
    return shotPopupId_ && std::strcmp(shotPopupId_, popupId) == 0;
}

f32 Ribbon::RowLabelWidth(const char* const* keys, i32 count) const {
    f32 w = 0.0f;
    for (i32 i = 0; i < count; ++i)
        w = std::max(w, ImGui::CalcTextSize(i18n::tr(keys[i])).x);
    return w + ImGui::GetStyle().ItemSpacing.x;
}

void Ribbon::Build() {
    BuildRail();

    if (ui::BeginRibbon()) {
        if (ui::BeginRibbonMenus()) {
            menus_.BuildStrip();
            // Background work nobody asked for — the client-database prewarm —
            // reports here instead of taking the screen.
            tools::DrawProgressStatus(ctx_.app.Tasks());
            // The one place the whole path of what is loaded is written out; the
            // document tabs carry the file name alone.
            const std::filesystem::path& model = ctx_.app.Documents().ActiveState().modelPath;
            if (!model.empty())
                ui::RibbonMenuStatus(io::PathToUtf8(model).c_str());
            ui::EndRibbonMenus();
        }
        BuildDocumentGroup();
        BuildPlaybackGroup();
        BuildSceneGroup();
        BuildLookGroup();
        BuildToolsGroup();
    }
    ui::EndRibbon();
}

// The application tile heads the rail and the mode tabs run down from it. The
// tile is a menu rather than a mode: opening a model is something every mode
// does. It wears the app's own icon out of the font atlas, and falls back to the
// drawn snowflake if that failed to bake.
void Ribbon::BuildRail() {
    if (ui::BeginRibbonRail()) {
        // Per frame, not cached: a rebuilt atlas moves the rectangle.
        ui::IconImage logo;
        logo.valid = ui::AppIconUV(&logo.uv0, &logo.uv1);
        if (logo.valid)
            logo.tex = ImGui::GetIO().Fonts->TexRef;

        const bool openFile = ui::RailTile("##file", ui::Icon::Logo, i18n::tr("menu.file"), logo);
        const ui::PopupAnchor anchor = ui::ItemPopupAnchor();
        if (openFile)
            ImGui::OpenPopup("##filemenu");
        if (ShotHolds("##file"))
            ImGui::OpenPopup("##filemenu", ImGuiPopupFlags_NoReopen);
        ui::SetNextPopupUnder(anchor);
        if (ImGui::BeginPopup("##filemenu")) {
            menus_.BuildFileItems();
            ImGui::EndPopup();
        }
        for (const ModeTab& tab : kModes) {
            if (ui::RailTab(tab.id, tab.icon, i18n::tr(tab.key), mode_ == tab.mode))
                mode_ = tab.mode;
        }
    }
    ui::EndRibbonRail();
}

void Ribbon::BuildDocumentGroup() {
    ViewerApp& app = ctx_.app;
    ui::RibbonGroup(i18n::tr("ribbon.group.document"), ui::Icon::Open, [&] {
        if (ui::RibbonButton("##open", ui::Icon::Open, i18n::tr("ribbon.open")))
            openDialog_.PickAndOpen();
        Tip("menu.file.open");

        // Save As writes a model back in its own format: MDX/MDL, a `.m3` through
        // the M3 writer, or an effect copied verbatim. A `.m2` has no writer, so the
        // button greys out rather than opening a dialog that can only fail at the
        // end — and it asks the SOURCE, because a `.wem` opened as any of these is
        // that model.
        const ModelCapabilities caps = app.Exports().Capabilities();
        if (ui::RibbonButton("##saveas", ui::Icon::Save, i18n::tr("ribbon.save"), caps.saveAsMdx || caps.saveM3))
            exportDialogs_.SaveAs();
        Tip("menu.file.save_as");

        // Separate from Save As: everything behind this one CONVERTS. Every model
        // this build draws can be written to WEM, which is why the button is gated
        // on the model's source; the game targets keep their own gates inside.
        const bool openExport =
            ui::RibbonButton("##export", ui::Icon::Export, i18n::tr("ribbon.export"), caps.exportWem);
        const ui::PopupAnchor exportAnchor = ui::ItemPopupAnchor();
        if (openExport)
            ImGui::OpenPopup("##exportmenu");
        if (ShotHolds("##export"))
            ImGui::OpenPopup("##exportmenu", ImGuiPopupFlags_NoReopen);
        Tip("menu.file.export");
        ui::SetNextPopupUnder(exportAnchor);
        if (ImGui::BeginPopup("##exportmenu")) {
            if (ImGui::MenuItem(i18n::tr("menu.file.export_wem"), nullptr, false, caps.exportWem))
                exportDialogs_.ExportWem();
            // A model that IS Warcraft III uses Save As, which does not have to
            // derive a material set it already carries; the M3 row is the same one
            // game over.
            if (ImGui::MenuItem(i18n::tr("menu.file.export_mdx"), nullptr, false, caps.exportMdx))
                exportDialogs_.ExportMdx();
            if (ImGui::MenuItem(i18n::tr("menu.file.export_m3"), nullptr, false, caps.exportM3))
                exportDialogs_.ExportM3();
            // And out of the family altogether, for Blender and everything else.
            if (ImGui::MenuItem(i18n::tr("menu.file.export_gltf"), nullptr, false, caps.exportGltf))
                exportDialogs_.ExportGltf();
            ImGui::EndPopup();
        }

        const bool hasModel = !app.Documents().ActiveState().modelPath.empty();
        const bool hasAnims = hasModel && !app.Playback().SequenceNames().empty();
        if (ui::RibbonButton("##frames", ui::Icon::Frames, i18n::tr("ribbon.frames"), hasAnims,
                             exportWindow_.IsOpen())) {
            if (exportWindow_.IsOpen())
                exportWindow_.Close();
            else
                exportWindow_.Open(app.Playback().ActiveSequence());
        }
        Tip("menu.file.export_frames");
    });
}

// The scene clock is what every profile advances on — animation, particles,
// ribbons and corn-fx alike — so one pair of buttons covers every format,
// including one with no sequence list at all.
void Ribbon::BuildPlaybackGroup() {
    PlaybackController& playback = ctx_.app.Playback();
    ui::RibbonGroup(i18n::tr("ribbon.group.playback"), ui::Icon::Play, [&] {
        const bool paused = playback.IsPaused();
        if (ui::RibbonButton("##transport.toggle", paused ? ui::Icon::Play : ui::Icon::Pause,
                             i18n::tr(paused ? "toolbar.play" : "toolbar.pause")))
            playback.SetPaused(!paused);
        Tip(paused ? "toolbar.play.tip" : "toolbar.pause.tip");

        if (ui::RibbonButton("##transport.restart", ui::Icon::Restart, i18n::tr("toolbar.restart")))
            playback.RestartPlayback();
        Tip("toolbar.restart.tip");

        // Beside the transport because it is the rest of that control: an `.m3`
        // needs several plays at once plus the global loops it starts itself.
        if (const Sc2AnimationFiles* sc2 = ctx_.app.Features().Sc2(); sc2 && sc2->CanAttach()) {
            if (ui::RibbonButton("##animfiles", ui::Icon::Tracks, i18n::tr("toolbar.animfiles"), true,
                                 animationWindow_.IsOpen()))
                animationWindow_.SetOpen(!animationWindow_.IsOpen());
            Tip("toolbar.animfiles.tip");
        }
    });
}

void Ribbon::BuildSceneGroup() {
    PlaybackController& playback = ctx_.app.Playback();
    const auto& seqs = playback.SequenceNames();
    static constexpr std::array<const char*, 2> kRowKeys = {"toolbar.animation", "toolbar.camera"};

    ui::RibbonGroup(i18n::tr("ribbon.group.scene"), ui::Icon::Cube, [&] {
        ui::BeginRibbonRows(seqs.empty() ? 1 : 2,
                            RowLabelWidth(kRowKeys.data(), static_cast<i32>(kRowKeys.size())));

        if (!seqs.empty()) {
            const i32 sel = playback.ActiveSequence();
            ui::RibbonRow(i18n::tr("toolbar.animation"));
            ImGui::SetNextItemWidth(ui::kSequenceComboWidth);
            if (ImGui::BeginCombo("##animation",
                                  seqs[std::clamp(sel, 0, static_cast<i32>(seqs.size()) - 1)].c_str())) {
                for (i32 i = 0; i < static_cast<i32>(seqs.size()); ++i) {
                    const bool isSel = (i == sel);
                    if (ImGui::Selectable(seqs[i].c_str(), isSel))
                        playback.SelectSequence(i);
                    if (isSel)
                        ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
        }

        const auto& presets = playback.CameraPresets();
        const i32 active = playback.ActiveCameraPreset().value_or(-1);
        const char* preview = (active < 0 || active >= static_cast<i32>(presets.size()))
                                  ? i18n::tr("toolbar.camera.free")
                                  : presets[static_cast<usize>(active)].name.c_str();
        ui::RibbonRow(i18n::tr("toolbar.camera"));
        ImGui::SetNextItemWidth(ui::kSequenceComboWidth);
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

        ui::EndRibbonRows();
    });
}

void Ribbon::BuildLookGroup() {
    auto& settings = ctx_.app.Service().Settings();
    WowAppearance* wow = ctx_.app.Features().Wow();
    const bool hasSkins = wow && !wow->SkinNames().empty();
    static constexpr std::array<const char*, 3> kRowKeys = {"toolbar.team", "toolbar.lighting", "toolbar.skin"};

    ui::RibbonGroup(i18n::tr("ribbon.group.look"), ui::Icon::Person, [&] {
        ui::BeginRibbonRows(hasSkins ? 3 : 2, RowLabelWidth(kRowKeys.data(), static_cast<i32>(kRowKeys.size())));

        renderer::model::Actor* focus = ctx_.app.Playback().FocusActor();
        // Red when nothing is loaded, which is what a fresh actor wears.
        tools::Rgb8 team = tools::UnpackBgr(focus ? focus->teamColor : 0x000000FFu);
        ui::RibbonRow(i18n::tr("toolbar.team"));
        if (ui::ColorEditRgb8("##team", team, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel) && focus)
            focus->SetTeamColor(team.r, team.g, team.b);

        i32 lighting = static_cast<i32>(settings.GetLightingMode());
        ui::RibbonRow(i18n::tr("toolbar.lighting"));
        ImGui::SetNextItemWidth(ui::kLightingComboWidth);
        if (ui::KeyCombo("##lighting", lighting, kLightingKeys)) {
            settings.SetLightingMode(static_cast<LightingMode>(lighting));
            ctx_.MarkSettingsDirty();
        }

        if (hasSkins)
            BuildWowControls(*wow);

        ui::EndRibbonRows();

        // ---- Character customisation ----
        // A dozen options behind one button rather than a dozen combos: a character
        // offers skin, face, hair, beard, eyes and more, and no strip has room for
        // that.
        if (wow && !wow->CharacterOptions().empty()) {
            const auto options = wow->CharacterOptions();
            const bool openCustomize =
                ui::RibbonButton("##customizebtn", ui::Icon::Person, i18n::tr("toolbar.customize"));
            const ui::PopupAnchor anchor = ui::ItemPopupAnchor();
            if (openCustomize)
                ImGui::OpenPopup("##customize");
            if (ShotHolds("##customize"))
                ImGui::OpenPopup("##customize", ImGuiPopupFlags_NoReopen);
            ui::SetNextPopupUnder(anchor);
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
                        wow->SetCharacterChoice(opt.optionId, static_cast<u32>(sel));
                }
                ImGui::EndPopup();
            }
        }
        BuildD3Equip();
    });
}

// A creature model leaves its skin blank for the game to fill; this is which
// fill. Absent for every other model, which is most of them.
void Ribbon::BuildWowControls(WowAppearance& wow) {
    const auto skins = wow.SkinNames();
    const u32 sel = wow.Skin() % static_cast<u32>(skins.size());
    ui::RibbonRow(i18n::tr("toolbar.skin"));
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
}

// The Diablo III half of the same idea, and the opposite problem: a `.m2`
// character leaves its geosets blank for the game to fill, while a `.app` ships
// every armour variant at once for the game to pick between. Absent for every
// model that is not a player character.
void Ribbon::BuildD3Equip() {
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
    const bool openEquip = ui::RibbonButton("##equipbtn", ui::Icon::Person, i18n::tr("toolbar.equip"));
    const ui::PopupAnchor anchor = ui::ItemPopupAnchor();
    if (openEquip)
        ImGui::OpenPopup("##d3equip");
    if (ShotHolds("##d3equip"))
        ImGui::OpenPopup("##d3equip", ImGuiPopupFlags_NoReopen);
    Tip("toolbar.equip.tip");
    ui::SetNextPopupUnder(anchor);
    if (ImGui::BeginPopup("##d3equip")) {
        d3Equip_.Build(*d3, slots);
        ImGui::EndPopup();
    }
}

void Ribbon::BuildToolsGroup() {
    ui::RibbonGroup(i18n::tr("ribbon.group.tools"), ui::Icon::Settings, [&] {
        const bool explorerOpen = ctx_.app.Explorer().IsOpen();
        if (ui::RibbonButton("##explorer", ui::Icon::Storage, i18n::tr("ribbon.explorer"), true, explorerOpen))
            ctx_.app.Explorer().SetOpen(!explorerOpen);
        Tip("menu.tools.storage_explorer");

        if (ui::RibbonButton("##settingsbtn", ui::Icon::Settings, i18n::tr("menu.settings")))
            settings_.Open();
    });
}

} // namespace whiteout::flakes
