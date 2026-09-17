#include "ui/setting_rows.h"

#include "localization.h"
#include "ui/ui_metrics.h"

#include <imgui.h>

namespace whiteout::flakes::ui {

bool SettingCheckbox(renderer::RenderSettings& settings, const settings::BoolSetting& setting) {
    bool on = (settings.*setting.get)();
    const bool changed = ImGui::Checkbox(i18n::tr(setting.labelKey), &on);
    if (changed)
        (settings.*setting.set)(on);
    if (setting.tipKey && ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", i18n::tr(setting.tipKey));
    return changed;
}

bool SettingSlider(renderer::RenderSettings& settings, const settings::FloatSetting& setting) {
    f32 value = (settings.*setting.get)();
    if (setting.fieldWidth)
        ImGui::SetNextItemWidth(kSettingsFieldWidth);
    if (!ImGui::SliderFloat(i18n::tr(setting.labelKey), &value, setting.min, setting.max, setting.format))
        return false;
    (settings.*setting.set)(value);
    return true;
}

} // namespace whiteout::flakes::ui
