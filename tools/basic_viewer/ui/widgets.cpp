#include "ui/widgets.h"

#include "localization.h"
#include "ui/ui_metrics.h"

#include <imgui_stdlib.h>
#include <nfd.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace whiteout::flakes::ui {

bool KeyCombo(const char* label, i32& index, std::span<const char* const> keys) {
    std::vector<const char*> items;
    items.reserve(keys.size());
    for (const char* key : keys)
        items.push_back(i18n::tr(key));
    return ImGui::Combo(label, &index, items.data(), static_cast<i32>(items.size()));
}

bool ColorEditRgb8(const char* label, tools::Rgb8& color, ImGuiColorEditFlags flags, bool round) {
    std::array<f32, 3> rgb = tools::ToUnitRgb(color);
    if (!ImGui::ColorEdit3(label, rgb.data(), flags))
        return false;
    color = round ? tools::FromUnitRgbRounded(rgb.data()) : tools::FromUnitRgbTruncated(rgb.data());
    return true;
}

bool PathRow(const PathRowSpec& spec, std::string& path, const std::string& resetTo) {
    bool commit = false;
    ImGui::SetNextItemWidth(-spec.reserve);
    ImGui::InputText(spec.inputId, &path);
    if (ImGui::IsItemDeactivatedAfterEdit())
        commit = true;
    ImGui::SameLine();
    if (ImGui::Button(i18n::tr(spec.browseKey))) {
        NFD::UniquePathU8 outPath;
        nfdresult_t picked = NFD_CANCEL;
        if (spec.filterSpec) {
            nfdu8filteritem_t filter[1] = {{spec.filterName, spec.filterSpec}};
            picked = NFD::OpenDialog(outPath, filter, 1);
        } else {
            picked = NFD::PickFolder(outPath);
        }
        if (picked == NFD_OKAY) {
            path = outPath.get();
            commit = true;
        }
    }
    ImGui::SameLine();
    if (ImGui::Button(i18n::tr(spec.resetKey))) {
        path = resetTo;
        commit = true;
    }
    ImGui::SameLine();
    ImGui::TextUnformatted(spec.label);
    return commit;
}

void HelpMarker(const char* text) {
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 28.0f);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

void Warn(ImU32 colour, const char* text) {
    ImGui::PushStyleColor(ImGuiCol_Text, colour);
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
}

void ErrorText(const char* caption, const char* detail) {
    ImGui::TextColored(ImVec4(kErrorText.r, kErrorText.g, kErrorText.b, kErrorText.a), "%s: %s", caption, detail);
}

} // namespace whiteout::flakes::ui
