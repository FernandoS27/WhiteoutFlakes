#pragma once

// ============================================================================
// The shape every "pick a file, then confirm its options" dialog shares: a
// target chosen in the native dialog, a modal centred on the viewport that
// draws the options, a confirm button and Cancel.
//
// The options persist across openings, as the dialogs' own members did, so a
// second export starts from the first one's choices.
// ============================================================================

#include "localization.h"
#include "ui/ui_metrics.h"

#include <imgui.h>

#include <filesystem>
#include <functional>
#include <utility>

namespace whiteout::flakes::ui {

template <class Options>
class OptionsModal {
public:
    /// Open the modal on the next Draw for @p target.
    void Open(std::filesystem::path target) {
        target_ = std::move(target);
        openRequested_ = true;
    }
    bool Pending() const {
        return !target_.empty();
    }
    const std::filesystem::path& Target() const {
        return target_;
    }
    Options& Values() {
        return options_;
    }

    /// @p rows draws the options. @p confirm runs on the confirm button and
    /// returns false to keep the modal open — a save that failed and says why.
    void Draw(const char* titleKey, const char* confirmKey, const std::function<void(Options&)>& rows,
              const std::function<bool(const std::filesystem::path&, const Options&)>& confirm) {
        if (openRequested_) {
            ImGui::OpenPopup(i18n::tr(titleKey));
            openRequested_ = false;
        }
        if (!Pending())
            return;

        const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
        ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        if (!ImGui::BeginPopupModal(i18n::tr(titleKey), nullptr, ImGuiWindowFlags_AlwaysAutoResize))
            return;

        rows(options_);

        ImGui::Separator();
        if (ImGui::Button(i18n::tr(confirmKey), ImVec2(kDialogButtonWidth, 0)) && confirm(target_, options_)) {
            target_.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button(i18n::tr("app.cancel"), ImVec2(kDialogCancelWidth, 0))) {
            target_.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

private:
    std::filesystem::path target_;
    bool openRequested_ = false;
    Options options_{};
};

} // namespace whiteout::flakes::ui
