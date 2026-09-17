#include "ui/tab_bar.h"

#include "app/viewer_app.h"
#include "imgui_ribbon.h"
#include "ui/ui_metrics.h"

#include <imgui.h>

namespace whiteout::flakes {

void BuildTabBar(ViewerApp& app) {
    DocumentManager& documents = app.Documents();
    if (documents.Count() <= 0)
        return;

    // Directly beneath the ribbon and right of its rail, over the top of the 3D
    // view: the tabs belong to the content area, not to the ribbon.
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const ui::RibbonLayout ribbon = ui::RibbonMetrics();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + ribbon.railW, vp->WorkPos.y + ribbon.topH));
    ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x - ribbon.railW, ImGui::GetFrameHeight() + ui::kStripPadding));
    ImGuiWindowFlags wf = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                          ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar |
                          ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    if (!ImGui::Begin("##tabbar", nullptr, wf)) {
        ImGui::End();
        ImGui::PopStyleVar(2);
        return;
    }

    ImGuiTabBarFlags tbFlags =
        ImGuiTabBarFlags_AutoSelectNewTabs | ImGuiTabBarFlags_Reorderable | ImGuiTabBarFlags_FittingPolicyScroll;
    if (ImGui::BeginTabBar("##documents", tbFlags)) {
        // After an app-driven change of the active tab (a command-line open,
        // File ▸ Open, a close handing focus to a neighbour) the tab bar would
        // default to its first tab. Force-select the app's tab for that one
        // frame, and skip following ImGui's selection, so a transient first-tab
        // selection cannot snap the active document back.
        const std::optional<i32> forceSelect = documents.ConsumePendingTabSelect();
        i32 toClose = -1;
        for (i32 i = 0; i < documents.Count(); ++i) {
            bool open = true;
            const ImGuiTabItemFlags flags = (forceSelect == i) ? ImGuiTabItemFlags_SetSelected : 0;
            // PushID disambiguates tabs whose labels (file stems) collide.
            ImGui::PushID(i);
            if (ImGui::BeginTabItem(documents.Title(i).c_str(), &open, flags)) {
                // True for the selected tab: follow the user's click.
                if (!forceSelect && documents.ActiveIndex() != i)
                    app.ActivateDocument(i);
                ImGui::EndTabItem();
            }
            ImGui::PopID();
            if (!open)
                toClose = i; // the (x) was clicked
        }
        ImGui::EndTabBar();
        if (toClose >= 0)
            app.CloseDocument(toClose);
    }

    ImGui::End();
    ImGui::PopStyleVar(2);
}

} // namespace whiteout::flakes
