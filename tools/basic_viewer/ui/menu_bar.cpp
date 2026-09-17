#include "ui/menu_bar.h"

#include "app/viewer_app.h"
#include "capture/export_window.h"
#include "localization.h"
#include "renderer/assets/replaceable_texture_manager.h"
#include "renderer/debug/debug_renderer.h"
#include "renderer/model/model_instance.h"
#include "renderer/render_service.h"
#include "ui/export_dialogs.h"
#include "ui/open_dialog.h"
#include "settings/setting_descriptors.h"
#include "ui/settings_window.h"
#include "ui/ui_context.h"
#include "whiteout/flakes/display.h"
#include "whiteout/flakes/util/replaceable_paths.h"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <optional>

namespace whiteout::flakes {

namespace {

using renderer::RenderSettings;

// Every debug view, in menu order, with the label it wears in the PBR and the
// legacy menu. Which of the two a model gets is the renderer's call
// (DebugFamilyOf); which views a family lists is DebugViewInFamily.
struct DebugViewItem {
    DebugView view;
    const char* pbrKey;
    const char* legacyKey;
};
constexpr std::array<DebugViewItem, 23> kDebugViews = {{
    {DebugView::Off, "debugvis.off", "debugvis.off"},
    {DebugView::Albedo, "debugvis.albedo", "debugvis.diffuse"},
    {DebugView::Normal, "debugvis.world_normal", "debugvis.normal_mapped"},
    {DebugView::VertexNormal, "debugvis.vertex_normal", "debugvis.vertex_normal"},
    {DebugView::Roughness, "debugvis.roughness", "debugvis.roughness"},
    {DebugView::Metalness, "debugvis.metalness", "debugvis.metalness"},
    {DebugView::MaterialAo, "debugvis.material_ao", "debugvis.material_ao"},
    {DebugView::Specular, "debugvis.specular", "debugvis.specular"},
    {DebugView::Gloss, "debugvis.gloss", "debugvis.gloss"},
    {DebugView::Emissive, "debugvis.emissive", "debugvis.emissive"},
    {DebugView::TeamMask, "debugvis.team_mask", "debugvis.team_mask"},
    {DebugView::VertexColor, "debugvis.vertex_color", "debugvis.vertex_color"},
    {DebugView::Opacity, "debugvis.opacity", "debugvis.opacity"},
    {DebugView::TextureMip, "debugvis.lod_heatmap", "debugvis.lod_heatmap"},
    {DebugView::LightCount, "debugvis.light_count", "debugvis.light_count"},
    {DebugView::LightingWhite, "debugvis.shading_white", "debugvis.shading_white"},
    {DebugView::LightingGrey, "debugvis.shading_grey", "debugvis.shading_grey"},
    {DebugView::SpecularOnly, "debugvis.specular_only", "debugvis.specular_only"},
    {DebugView::NoOrm, "debugvis.no_orm", "debugvis.no_orm"},
    {DebugView::AoOnly, "debugvis.ao_only", "debugvis.ao_only"},
    // Geometry, for both families. The menu puts a separator before these.
    {DebugView::Wireframe, "debugvis.wireframe", "debugvis.wireframe"},
    {DebugView::WireframeVertices, "debugvis.wireframe_vertices", "debugvis.wireframe_vertices"},
    {DebugView::WireframeTeamColor, "debugvis.wireframe_team_color", "debugvis.wireframe_team_color"},
}};

/// Auto, then an explicit level: index 0 is "no override".
constexpr std::array<const char*, 5> kLodKeys = {"lod.auto", "lod.0", "lod.1", "lod.2", "lod.3"};
constexpr i32 kHighestLod = 3;

struct ArtTierChoice {
    const char* key;
    std::optional<Wc3ArtTier> tier;
};
constexpr std::array<ArtTierChoice, 4> kArtTierChoices = {{
    {"menu.view.art_tier_auto", std::nullopt},
    {"menu.view.art_tier_classic", Wc3ArtTier::Classic},
    {"menu.view.art_tier_reforged", Wc3ArtTier::Reforged},
    {"menu.view.art_tier_definitive", Wc3ArtTier::Definitive},
}};

} // namespace

MenuBar::MenuBar(UiContext& ctx, OpenDialog& openDialog, ExportDialogs& exportDialogs, ExportWindow& exportWindow,
                 SettingsWindow& settings)
    : ctx_(ctx), openDialog_(openDialog), exportDialogs_(exportDialogs), exportWindow_(exportWindow),
      settings_(settings) {}

void MenuBar::BuildStrip() {
    auto& render = ctx_.app.Service().Settings();
    DisplayFlags df = render.GetDisplayFlags();
    bool displayChanged = false;

    // Every frame: a window appearing takes focus and closes popups.
    if (shotMenuKey_)
        ImGui::OpenPopup(i18n::tr(shotMenuKey_), ImGuiPopupFlags_NoReopen);

    if (ImGui::BeginMenu(i18n::tr("menu.view"))) {
        displayChanged |= ImGui::MenuItem(i18n::tr("menu.view.grid"), nullptr, &df.showGrid);
        displayChanged |= ImGui::MenuItem(i18n::tr("menu.view.particles"), nullptr, &df.showParticles);
        displayChanged |= ImGui::MenuItem(i18n::tr("menu.view.ribbons"), nullptr, &df.showRibbons);
        displayChanged |= ImGui::MenuItem(i18n::tr("menu.view.events"), nullptr, &df.showEvents);
        ImGui::MenuItem(i18n::tr("menu.view.viewcube"), nullptr, &showViewCube_);
        BuildViewMenu();
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu(i18n::tr("menu.debug"))) {
        displayChanged |= ImGui::MenuItem(i18n::tr("menu.debug.collisions"), nullptr, &df.showCollisions);
        displayChanged |= ImGui::MenuItem(i18n::tr("menu.debug.lights"), nullptr, &df.showLights);
        BuildDebugMenu();
        ImGui::EndMenu();
    }

    // Settings rides with the Storage Explorer rather than sitting on the strip
    // by itself: the ribbon's Tools group is where both are one click away.
    if (ImGui::BeginMenu(i18n::tr("menu.tools"))) {
        bool explorerOpen = ctx_.app.Explorer().IsOpen();
        if (ImGui::MenuItem(i18n::tr("menu.tools.storage_explorer"), nullptr, &explorerOpen))
            ctx_.app.Explorer().SetOpen(explorerOpen);
        if (ImGui::MenuItem(i18n::tr("menu.settings")))
            settings_.Open();
        ImGui::EndMenu();
    }

    // Endonyms in their own script, not translated; the bundled Noto fonts
    // cover every entry. Switching swaps the in-memory catalog, so the whole
    // UI re-localises next frame.
    if (ImGui::BeginMenu(i18n::tr("menu.language"))) {
        const i18n::Language cur = i18n::Localizer::instance().current();
        for (const auto& e : i18n::languages()) {
            if (ImGui::MenuItem(e.endonym, nullptr, e.lang == cur)) {
                i18n::Localizer::instance().setLanguage(e.lang);
                ctx_.MarkSettingsDirty();
            }
        }
        ImGui::EndMenu();
    }

    if (displayChanged) {
        render.SetDisplayFlags(df);
        ctx_.MarkSettingsDirty();
    }
}

void MenuBar::BuildFileItems() {
    ViewerApp& app = ctx_.app;
    if (ImGui::MenuItem(i18n::tr("menu.file.open"), "Ctrl+O"))
        openDialog_.PickAndOpen();
    // Save As writes a model back in its own format: MDX/MDL, a `.m3` through
    // the M3 writer, or an effect copied verbatim. A `.m2` has no writer, so the
    // item greys out rather than opening a dialog that can only fail at the end —
    // and it asks the SOURCE, because a `.wem` opened as any of these is that model.
    const ModelCapabilities caps = app.Exports().Capabilities();
    if (ImGui::MenuItem(i18n::tr("menu.file.save_as"), "Ctrl+Shift+S", false, caps.saveAsMdx || caps.saveM3))
        exportDialogs_.SaveAs();
    // Separate from Save As: everything in here CONVERTS. Every model this build
    // draws can be written to WEM, which is why the submenu is gated on the
    // model's source; the game targets keep their own gates inside.
    if (ImGui::BeginMenu(i18n::tr("menu.file.export"), caps.exportWem)) {
        if (ImGui::MenuItem(i18n::tr("menu.file.export_wem"), nullptr, false, caps.exportWem))
            exportDialogs_.ExportWem();
        // A model that IS Warcraft III uses Save As, which does not have to derive
        // a material set it already carries; the M3 row is the same one game over.
        if (ImGui::MenuItem(i18n::tr("menu.file.export_mdx"), nullptr, false, caps.exportMdx))
            exportDialogs_.ExportMdx();
        if (ImGui::MenuItem(i18n::tr("menu.file.export_m3"), nullptr, false, caps.exportM3))
            exportDialogs_.ExportM3();
        // And out of the family altogether, for Blender and everything else.
        if (ImGui::MenuItem(i18n::tr("menu.file.export_gltf"), nullptr, false, caps.exportGltf))
            exportDialogs_.ExportGltf();
        ImGui::EndMenu();
    }
    const bool hasModel = !app.Documents().ActiveState().modelPath.empty();
    const bool hasAnims = hasModel && !app.Playback().SequenceNames().empty();
    if (ImGui::MenuItem(i18n::tr("menu.file.export_frames"), nullptr, exportWindow_.IsOpen(), hasAnims)) {
        if (exportWindow_.IsOpen())
            exportWindow_.Close();
        else
            exportWindow_.Open(app.Playback().ActiveSequence());
    }
    ImGui::Separator();
    if (ImGui::MenuItem(i18n::tr("menu.file.exit")))
        app.Window().RequestClose();
}

void MenuBar::BuildViewMenu() {
    ViewerApp& app = ctx_.app;
    ImGui::Separator();
    // "Reforged Graphics": the HD pipeline for every model.
    bool reforged = app.Loader().ForceHd();
    if (ImGui::MenuItem(i18n::tr("menu.view.reforged"), nullptr, &reforged)) {
        app.Loader().SetForceHd(reforged);
        ctx_.MarkSettingsDirty();
    }
    // Which of Warcraft III's three CASC overlays a read resolves through.
    // Separate from the render mode because 3.0.0 made them separate questions:
    // Reforged and Definitive are different files drawn the same way. "Follow
    // Render Mode" is the pre-3.0.0 behaviour (SD ⇒ Classic, HD ⇒ Reforged), and
    // a storage browse still overrides it per model.
    if (ImGui::BeginMenu(i18n::tr("menu.view.art_tier"))) {
        const std::optional<Wc3ArtTier> cur = app.Service().Settings().GetArtTier();
        for (const ArtTierChoice& c : kArtTierChoices) {
            if (ImGui::MenuItem(i18n::tr(c.key), nullptr, cur == c.tier) && cur != c.tier) {
                app.Loader().SetArtTier(c.tier);
                ctx_.MarkSettingsDirty();
            }
        }
        ImGui::EndMenu();
    }

    ImGui::Separator();
    if (ImGui::BeginMenu(i18n::tr("menu.view.tileset"))) {
        const i32 count = static_cast<i32>(io::Tileset::Count);
        const i32 cur = static_cast<i32>(io::GetCurrentTileset());
        for (i32 i = 0; i < count; ++i) {
            if (ImGui::MenuItem(io::TilesetName(static_cast<io::Tileset>(i)), nullptr, i == cur)) {
                app.Service().Replaceables().SetTileset(static_cast<io::Tileset>(i));
                ctx_.MarkSettingsDirty();
            }
        }
        ImGui::EndMenu();
    }
}

void MenuBar::BuildDebugMenu() {
    ViewerApp& app = ctx_.app;
    auto& render = app.Service().Settings();

    if (ImGui::BeginMenu(i18n::tr("menu.debug.physics"))) {
        BuildPhysicsMenu();
        ImGui::EndMenu();
    }
    ImGui::Separator();

    if (ImGui::BeginMenu(i18n::tr("menu.debug.debugview"))) {
        // The views the focused model's materials can answer: a Reforged model
        // drawn in HD gets the PBR set, everything else the legacy one.
        const DebugViewFamily family = app.Service().DebugFamilyOf(app.Documents().ActiveState().focusActor);
        ImGui::TextDisabled("%s", i18n::tr(family == DebugViewFamily::Pbr ? "debugvis.family_pbr"
                                                                          : "debugvis.family_legacy"));
        ImGui::Separator();
        const DebugView cur = render.GetDebugView();
        for (const auto& item : kDebugViews) {
            if (!DebugViewInFamily(item.view, family))
                continue;
            if (item.view == DebugView::Wireframe)
                ImGui::Separator();
            const char* key = family == DebugViewFamily::Pbr ? item.pbrKey : item.legacyKey;
            if (ImGui::MenuItem(i18n::tr(key), nullptr, item.view == cur)) {
                render.SetDebugView(item.view);
                ctx_.MarkSettingsDirty();
            }
        }
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu(i18n::tr("menu.debug.lod"))) {
        const i32 cur = render.LodOverride();
        const i32 curIdx = (cur < 0) ? 0 : (1 + std::clamp(cur, 0, kHighestLod));
        for (i32 i = 0; i < static_cast<i32>(kLodKeys.size()); ++i) {
            if (ImGui::MenuItem(i18n::tr(kLodKeys[i]), nullptr, i == curIdx)) {
                render.SetLodOverride(i == 0 ? -1 : (i - 1));
                ctx_.MarkSettingsDirty();
            }
        }
        ImGui::EndMenu();
    }

    ImGui::Separator();
    ImGui::MenuItem(i18n::tr("menu.debug.log_console"), nullptr, &showLogConsole_);
}

void MenuBar::BuildPhysicsMenu() {
    auto& render = ctx_.app.Service().Settings();
    const auto toggle = [&](const settings::BoolSetting& setting) {
        const bool on = (render.*setting.get)();
        if (ImGui::MenuItem(i18n::tr(setting.labelKey), nullptr, on)) {
            (render.*setting.set)(!on);
            ctx_.MarkSettingsDirty();
        }
    };
    for (const settings::BoolSetting* overlay : settings::kPhysicsOverlays)
        toggle(*overlay);

    // Not an overlay: this one changes the picture. A cloth solver is only
    // judgeable as a difference, so the host needs a way to put the geoset back
    // on its skinning without a rebuild.
    ImGui::Separator();
    toggle(settings::kClothDeform);

    // Everything above draws; this one *runs*. D3 builds a ragdoll on a gameplay
    // event no model file carries, so the host is the only place it can come
    // from. Per-actor, so not saved, and greyed rather than hidden so a model
    // without a rig still shows the viewer has one.
    ImGui::Separator();
    D3Wardrobe* d3 = ctx_.app.Features().D3();
    const bool hasRig = d3 && d3->HasRagdoll();
    if (ImGui::MenuItem(i18n::tr("menu.debug.physics.ragdoll"), nullptr, hasRig && d3->Ragdoll(), hasRig))
        d3->SetRagdoll(!d3->Ragdoll());
}

} // namespace whiteout::flakes
