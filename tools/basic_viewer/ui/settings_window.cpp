#include "ui/settings_window.h"

#include "app/viewer_app.h"
#include "io/file_content_provider.h"
#include "localization.h"
#include "renderer/render_service.h"
#include "renderer/scene_manager.h"
#include "session/game_profiles.h"
#include "ui/ui_context.h"
#include "ui/ui_metrics.h"

#include <imgui.h>

namespace whiteout::flakes {

SettingsWindow::SettingsWindow(UiContext& ctx) : ctx_(ctx) {}

void SettingsWindow::ShowGeneralForShot() {
    open_ = true;
    generalPage_ = true;
}

void SettingsWindow::ShowProfileForShot(ProductId game, bool io) {
    open_ = true;
    generalPage_ = false;
    ctx_.app.Session().ShowSettingsProfile(game); // not SetSettingsProfile: that persists
    shotIoTab_ = io;
}

void SettingsWindow::Build() {
    if (!open_)
        return;
    ImGui::SetNextWindowSize(ImVec2(ui::kSettingsWindowWidth, ui::kSettingsWindowHeight), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(i18n::tr("settings.title"), &open_)) {
        ImGui::End();
        return;
    }

    // The profile being EDITED and the product the provider is SERVING are two
    // facts. They agree most of the time, and clicking a row moves only the
    // first — configuring a game is not a reason to go read it.
    StorageSession& session = ctx_.app.Session();
    io::FileContentProvider& provider = session.Provider();
    const ProductId game = session.SettingsProfile();
    const ProductId serving = provider.Game();

    ImGui::BeginChild("##profiles", ImVec2(ui::kSettingsProfileListWidth, 0.0f), ImGuiChildFlags_Borders);
    // Above the games, and not one of them: the background colour, the frame's
    // post chain and the backend hold whatever is loaded.
    if (ImGui::Selectable(i18n::tr("settings.profile.general"), generalPage_))
        generalPage_ = true;
    ImGui::Separator();
    ImGui::TextDisabled("%s", i18n::tr("settings.profile.header"));
    ImGui::Separator();
    for (const GameProfile& p : kGameProfiles) {
        const bool picked = !generalPage_ && p.product == game;
        if (ImGui::Selectable(p.displayName, picked) && !picked) {
            generalPage_ = false;
            // Which settings to show and which ini section to write, and nothing
            // else: no storage opens and no asset retries. A profile's install
            // opens when content from that game loads (StorageSession::FollowModelGame).
            session.SetSettingsProfile(p.product);
        }
        // A mark on the one whose storage is in use, so a page saying "nothing is
        // open" reads as legible rather than alarming.
        if (p.product == serving) {
            ImGui::SameLine();
            ImGui::TextDisabled("*");
        }
    }
    ImGui::EndChild();

    ImGui::SameLine();

    ImGui::BeginChild("##profilebody", ImVec2(0.0f, 0.0f));
    if (generalPage_) {
        // No tab bar: the shared page has no second page. IO is a game's
        // install, so it belongs to a game's row.
        BuildGeneralPage();
    } else if (ImGui::BeginTabBar("##SettingsTabs")) {
        if (ImGui::BeginTabItem(i18n::tr("settings.tab.general"))) {
            BuildProfilePage(game);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(i18n::tr("settings.tab.io"), nullptr, shotIoTab_ ? ImGuiTabItemFlags_SetSelected : 0)) {
            BuildIoTab(provider, game);
            ImGui::EndTabItem();
        }
        shotIoTab_ = false;
        ImGui::EndTabBar();
    }
    ImGui::EndChild();

    ImGui::End();
}

} // namespace whiteout::flakes
