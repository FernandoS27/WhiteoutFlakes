#include "ui/viewer_ui.h"

#include "app/viewer_app.h"
#include "imgui_viewcube.h"
#include "localization.h"
#include "log_console.h"
#include "progress_dialog.h"
#include "renderer/camera.h"
#include "renderer/render_service.h"
#include "renderer/scene_manager.h"
#include "settings_ini.h"
#include "ui/tab_bar.h"
#include "ui/ui_metrics.h"

#include <imgui.h>
#include <nfd.hpp>

namespace whiteout::flakes {

ViewerUI::ViewerUI(ViewerApp& app)
    : ctx_{app},
      exportWindow_(app),
      openDialog_(ctx_),
      exportDialogs_(ctx_),
      settings_(ctx_),
      animationWindow_(ctx_),
      toolbar_(ctx_, animationWindow_),
      menuBar_(ctx_, openDialog_, exportDialogs_, exportWindow_, settings_) {
    // NFD's init / quit can be reference-counted; once, at first UI
    // construction, matches its single-process expectations.
    NFD::Init();
}

void ViewerUI::BuildFrame() {
    ViewerApp& app = ctx_.app;
    menuBar_.Build();
    toolbar_.Build();
    BuildTabBar(app);
    if (menuBar_.ShowViewCube())
        BuildViewCube();
    settings_.Build();
    animationWindow_.Build();
    exportDialogs_.Build();
    openDialog_.Build();
    exportWindow_.Build();
    app.Explorer().BuildWindow();
    tools::LogConsole::Instance().DrawUi(&menuBar_.ShowLogConsole());
    // Last, so it lands over everything else — the point of a modal for work the
    // rest of the UI cannot usefully be clicked during.
    tools::DrawProgressModal(app.Tasks());

    if (ctx_.settingsDirty) {
        SaveSettingsIni(app.Service(), app.Playback().LoopNonLooping(), app.Loader().ForceHd(),
                        i18n::languageCode(i18n::Localizer::instance().current()));
        ctx_.settingsDirty = false;
    }
}

void ViewerUI::BuildViewCube() {
    const f32 stripH = ImGui::GetFrameHeight() + ui::kStripPadding;
    const f32 topOffset = stripH + (ctx_.app.Documents().Count() > 0 ? stripH : 0.0f);
    tools::DrawViewCube(ctx_.app.Service().Scene().Camera(), topOffset);
}

bool ViewerUI::OpenPanelForShot(std::string_view panel) {
    struct MenuShot {
        std::string_view name;
        const char* key;
    };
    constexpr MenuShot kMenus[] = {{"menu-file", "menu.file"},
                                   {"menu-view", "menu.view"},
                                   {"menu-debug", "menu.debug"},
                                   {"menu-tools", "menu.tools"},
                                   {"menu-language", "menu.language"}};
    struct SettingsShot {
        std::string_view name;
        ProductId game;
        bool io;
    };
    constexpr SettingsShot kSettings[] = {
        {"settings-wc3", ProductId::Wc3, false}, {"settings-wc3-io", ProductId::Wc3, true},
        {"settings-sc2", ProductId::Sc2, false}, {"settings-sc2-io", ProductId::Sc2, true},
        {"settings-wow", ProductId::Wow, false}, {"settings-wow-io", ProductId::Wow, true},
        {"settings-d3", ProductId::D3, false},   {"settings-d3-io", ProductId::D3, true},
    };

    ViewerApp& app = ctx_.app;
    if (panel == "main")
        return true;
    for (const MenuShot& m : kMenus) {
        if (panel == m.name) {
            menuBar_.HoldMenuOpen(m.key);
            return true;
        }
    }
    if (panel == "settings-general") {
        settings_.ShowGeneralForShot();
        return true;
    }
    for (const SettingsShot& s : kSettings) {
        if (panel == s.name) {
            settings_.ShowProfileForShot(s.game, s.io);
            return true;
        }
    }
    if (panel == "animation") {
        animationWindow_.SetOpen(true);
        const Sc2AnimationFiles* sc2 = app.Features().Sc2();
        return sc2 && sc2->CanAttach();
    }
    if (panel == "export") {
        exportWindow_.Open(app.Playback().ActiveSequence());
        return true;
    }
    if (panel == "d3-equip") {
        toolbar_.HoldPopupOpen("##d3equip");
        const D3Wardrobe* d3 = app.Features().D3();
        return d3 && !d3->CharacterSlots().empty();
    }
    if (panel == "wow-customize") {
        toolbar_.HoldPopupOpen("##customize");
        const WowAppearance* wow = app.Features().Wow();
        return wow && !wow->CharacterOptions().empty();
    }
    if (panel == "dialog-wem")
        return openDialog_.OpenForShot();
    return exportDialogs_.OpenForShot(panel);
}

} // namespace whiteout::flakes
