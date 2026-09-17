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

namespace {

void DrawToolbarIcon(ImDrawList* dl, ToolbarIcon icon, ImVec2 c, f32 s, ImU32 col) {
    constexpr f32 kPi = 3.14159265358979323846f;
    switch (icon) {
    case ToolbarIcon::Play: {
        // Nudged right: a triangle centred on its bounding box reads as sitting
        // too far left, because its mass is in the flat edge.
        c.x += s * 0.03f;
        const f32 w = s * 0.30f;
        const f32 h = s * 0.30f;
        dl->AddTriangleFilled(ImVec2(c.x - w * 0.6f, c.y - h), ImVec2(c.x + w, c.y),
                              ImVec2(c.x - w * 0.6f, c.y + h), col);
        break;
    }
    case ToolbarIcon::Pause: {
        const f32 bar = s * 0.12f;
        const f32 gap = s * 0.07f; // half the space between the two bars
        const f32 h = s * 0.30f;
        dl->AddRectFilled(ImVec2(c.x - gap - bar, c.y - h), ImVec2(c.x - gap, c.y + h), col, bar * 0.3f);
        dl->AddRectFilled(ImVec2(c.x + gap, c.y - h), ImVec2(c.x + gap + bar, c.y + h), col, bar * 0.3f);
        break;
    }
    case ToolbarIcon::Restart: {
        // The ↻ everything else uses for "play it again": most of a circle with
        // the gap across the top, and the head at the END of travel, so the
        // arrow points the way the stroke was going.
        const f32 r = s * 0.28f;
        const f32 th = std::max(1.5f, s * 0.095f);
        constexpr f32 kFrom = -0.28f * kPi;
        constexpr f32 kTo = 1.28f * kPi;
        dl->PathArcTo(c, r, kFrom, kTo);
        dl->PathStroke(col, ImDrawFlags_None, th);

        // Tangent at kTo, in the direction the arc was drawn, and its normal.
        // The head's base sits ON the arc's last point so the two read as one
        // stroke; longer than it is wide, or it looks like a blob.
        const ImVec2 end(c.x + r * std::cos(kTo), c.y + r * std::sin(kTo));
        const ImVec2 dir(-std::sin(kTo), std::cos(kTo));
        const ImVec2 nrm(-dir.y, dir.x);
        const f32 len = th * 2.3f;
        const f32 wide = th * 1.15f;
        dl->AddTriangleFilled(ImVec2(end.x + dir.x * len, end.y + dir.y * len),
                              ImVec2(end.x + nrm.x * wide, end.y + nrm.y * wide),
                              ImVec2(end.x - nrm.x * wide, end.y - nrm.y * wide), col);
        break;
    }
    case ToolbarIcon::Tracks: {
        // Three stacked bars of unequal length — the track list a timeline puts
        // beside its transport, which is what the window behind this button is.
        const f32 th = std::max(1.5f, s * 0.10f);
        const f32 x0 = c.x - s * 0.27f;
        const f32 w[3] = {s * 0.54f, s * 0.32f, s * 0.44f};
        for (i32 i = 0; i < 3; ++i) {
            const f32 y = c.y + (static_cast<f32>(i) - 1.0f) * s * 0.20f;
            dl->AddRectFilled(ImVec2(x0, y - th * 0.5f), ImVec2(x0 + w[i], y + th * 0.5f), col, th * 0.5f);
        }
        break;
    }
    }
}

} // namespace

bool IconButton(const char* id, ToolbarIcon icon, const char* nameKey, const char* tipKey, bool active) {
    const f32 side = ImGui::GetFrameHeight();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    if (active)
        ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    const bool clicked = ImGui::Button(id, ImVec2(side, side));
    if (active)
        ImGui::PopStyleColor();
    DrawToolbarIcon(ImGui::GetWindowDrawList(), icon, ImVec2(p.x + side * 0.5f, p.y + side * 0.5f), side,
                    ImGui::GetColorU32(ImGuiCol_Text));
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s\n%s", i18n::tr(nameKey), i18n::tr(tipKey));
    return clicked;
}

void ToolbarLabel(const char* key) {
    // Every caller follows a SameLine (the transport group opens the row), and a
    // second SameLine simply re-places the cursor with the wider spacing.
    // AlignTextToFramePadding drops the text onto the frame's baseline; without
    // it the caption rides high against a taller neighbour.
    ImGui::SameLine(0.0f, ImGui::GetStyle().ItemSpacing.x * 2.5f);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(i18n::tr(key));
    ImGui::SameLine();
}

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
