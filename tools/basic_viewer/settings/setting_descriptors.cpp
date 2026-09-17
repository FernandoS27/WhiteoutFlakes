#include "settings/setting_descriptors.h"

#include <algorithm>
#include <string>

namespace whiteout::flakes::settings {

namespace {

std::string KeyOf(std::string_view key) {
    std::string out(kDisplaySection);
    out += '.';
    out += key;
    return out;
}

} // namespace

void Load(RenderSettings& settings, const ini::IniMap& ini, const BoolSetting& setting) {
    if (setting.iniKey.empty())
        return;
    const std::string* stored = ini.Get(KeyOf(setting.iniKey));
    if (!stored)
        return;
    if (setting.strictOne) {
        (settings.*setting.set)(*stored == "1");
        return;
    }
    bool value = false;
    if (ini::ParseBool(*stored, value))
        (settings.*setting.set)(value);
}

void Load(RenderSettings& settings, const ini::IniMap& ini, const FloatSetting& setting) {
    const std::string* stored = ini.Get(KeyOf(setting.iniKey));
    f32 value = 0.0f;
    if (!stored || !ini::ParseFloat(*stored, value))
        return;
    (settings.*setting.set)(setting.clampOnLoad ? std::clamp(value, setting.min, setting.max) : value);
}

void Save(const RenderSettings& settings, ini::IniMap& ini, const BoolSetting& setting) {
    if (!setting.iniKey.empty())
        ini.Set(KeyOf(setting.iniKey), (settings.*setting.get)() ? "1" : "0");
}

void Save(const RenderSettings& settings, ini::IniMap& ini, const FloatSetting& setting) {
    ini.Set(KeyOf(setting.iniKey), ini::FloatToString((settings.*setting.get)()));
}

void Load(RenderSettings& settings, const ini::IniMap& ini, std::span<const BoolSetting* const> group) {
    for (const BoolSetting* setting : group)
        Load(settings, ini, *setting);
}

void Load(RenderSettings& settings, const ini::IniMap& ini, std::span<const FloatSetting* const> group) {
    for (const FloatSetting* setting : group)
        Load(settings, ini, *setting);
}

void Save(const RenderSettings& settings, ini::IniMap& ini, std::span<const BoolSetting* const> group) {
    for (const BoolSetting* setting : group)
        Save(settings, ini, *setting);
}

void Save(const RenderSettings& settings, ini::IniMap& ini, std::span<const FloatSetting* const> group) {
    for (const FloatSetting* setting : group)
        Save(settings, ini, *setting);
}

void Reset(RenderSettings& settings, const FloatSetting& setting) {
    (settings.*setting.set)(setting.resetValue);
}

} // namespace whiteout::flakes::settings
