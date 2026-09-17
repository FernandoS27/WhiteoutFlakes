#include "ui/open_dialog.h"

#include "app/viewer_app.h"
#include "io/wem/wem_import.h"
#include "localization.h"
#include "ui/file_dialogs.h"
#include "ui/ui_context.h"
#include "ui/ui_metrics.h"
#include "whiteout/flakes/util/path_utf8.h"

#include <imgui.h>

namespace whiteout::flakes {

OpenDialog::OpenDialog(UiContext& ctx) : ctx_(ctx) {}

void OpenDialog::PickAndOpen() {
    const std::optional<std::filesystem::path> picked = PickModelToOpen();
    if (!picked)
        return;
    if (auto document = ctx_.app.Loader().PeekWemDocument(*picked)) {
        Ask(*picked, std::move(document));
        // The document's own default preselected: the row a plain open would use.
        const auto preferred = io::DefaultWemProfile(document_->document);
        for (usize i = 0; i < options_.size(); ++i) {
            if (options_[i].profile == preferred) {
                selection_ = static_cast<i32>(i);
                break;
            }
        }
        return;
    }
    // Async: an `.m2` or `.m3` picked here may need a game install that is not
    // open yet, and that open is seconds long.
    ctx_.app.Loader().OpenModelAsync(*picked);
}

bool OpenDialog::OpenForShot() {
    const std::filesystem::path& path = ctx_.app.Documents().ActiveState().modelPath;
    auto document = ctx_.app.Loader().PeekWemDocument(path);
    if (!document)
        return false;
    Ask(path, std::move(document));
    return true;
}

void OpenDialog::Ask(const std::filesystem::path& path, std::shared_ptr<io::WemDocument> document) {
    document_ = std::move(document);
    path_ = path;
    options_ = io::WemProfileOptions(document_->document);
    selection_ = 0;
    openRequested_ = true;
}

void OpenDialog::Clear() {
    document_.reset();
    options_.clear();
}

// Every row the file could be opened as, in the order io::WemProfileOptions ranks
// them: carried first, derives after, and the ones this build cannot open last
// and disabled — with the reason, because a file that says "Diablo III" and a
// dialog with no Diablo III row reads as a bug.
void OpenDialog::Build() {
    if (openRequested_) {
        ImGui::OpenPopup(i18n::tr("dialog.wem.title"));
        openRequested_ = false;
    }
    if (!document_)
        return;

    const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal(i18n::tr("dialog.wem.title"), nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        return;

    ImGui::TextUnformatted(io::PathToUtf8(path_.filename()).c_str());
    ImGui::Separator();
    ImGui::TextUnformatted(i18n::tr("dialog.wem.prompt"));
    ImGui::Spacing();

    for (usize i = 0; i < options_.size(); ++i) {
        const io::WemProfileOption& option = options_[i];
        ImGui::BeginDisabled(!option.supported);
        if (ImGui::RadioButton(option.displayName, selection_ == static_cast<i32>(i)))
            selection_ = static_cast<i32>(i);
        ImGui::EndDisabled();

        // What the row costs, said on the row. "Derived" is the load-bearing one:
        // it is always lossy (§6.6), and the alternative to saying so is a model
        // that quietly renders as an approximation of itself.
        ImGui::SameLine();
        if (!option.supported)
            ImGui::TextDisabled("— %s", io::WemProfileUnsupportedReason(option.profile));
        else if (option.carried)
            ImGui::TextDisabled("— %s", i18n::tr(option.drawn ? "dialog.wem.carried" : "dialog.wem.carried_undrawn"));
        else
            ImGui::TextDisabled("— %s", i18n::tr("dialog.wem.derived"));
    }

    ImGui::Spacing();
    const bool canOpen = selection_ >= 0 && selection_ < static_cast<i32>(options_.size()) &&
                         options_[static_cast<usize>(selection_)].supported;
    ImGui::BeginDisabled(!canOpen);
    if (ImGui::Button(i18n::tr("dialog.wem.open"), ImVec2(ui::kDialogButtonWidth, 0))) {
        // Synchronous, unlike File ▸ Open's model path: the file is parsed, and
        // its textures resolve against whichever install the profile names,
        // which OpenWemAs opens on the way through.
        ctx_.app.Loader().OpenWemAs(path_, document_, options_[static_cast<usize>(selection_)].profile);
        Clear();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button(i18n::tr("app.cancel"), ImVec2(ui::kDialogButtonWidth, 0))) {
        Clear();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

} // namespace whiteout::flakes
