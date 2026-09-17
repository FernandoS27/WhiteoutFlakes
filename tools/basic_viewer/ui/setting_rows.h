#pragma once

// ============================================================================
// Settings page rows drawn from a setting's descriptor
// (settings/setting_descriptors.h): the label, tooltip, range and format come
// from the one place the ini load and save read them too.
// ============================================================================

#include "settings/setting_descriptors.h"

namespace whiteout::flakes::ui {

/// A checkbox, with the descriptor's tooltip. True when toggled.
bool SettingCheckbox(renderer::RenderSettings& settings, const settings::BoolSetting& setting);
/// A slider over the descriptor's range and format. True when moved.
bool SettingSlider(renderer::RenderSettings& settings, const settings::FloatSetting& setting);

} // namespace whiteout::flakes::ui
