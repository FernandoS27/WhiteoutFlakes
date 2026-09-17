#include "features/d3_outfit_presets.h"

#include "ini_file.h"

#include <cstdlib>
#include <set>

namespace whiteout::flakes {

namespace {

// A preset name becomes an ini section, and the section separator is '.'.
std::string PresetSection(std::string_view name) {
    std::string s(name);
    for (char& c : s)
        if (c == '.' || c == '[' || c == ']' || c == '=')
            c = '_';
    return s;
}

std::string SlotKey(const std::string& section, usize slot) {
    return section + ".Slot" + std::to_string(slot);
}

std::string DyeKey(const std::string& section, usize slot) {
    return section + ".Dye" + std::to_string(slot);
}

} // namespace

std::vector<std::string> ListD3OutfitPresets(const std::filesystem::path& file) {
    ini::IniMap map;
    map.Load(file);
    std::set<std::string> names;
    for (const auto& [key, value] : map.values) {
        const auto dot = key.rfind('.');
        if (dot != std::string::npos)
            names.insert(key.substr(0, dot));
    }
    return {names.begin(), names.end()};
}

D3OutfitPreset ReadD3OutfitPreset(const std::filesystem::path& file, std::string_view name) {
    ini::IniMap map;
    map.Load(file);
    const std::string section = PresetSection(name);
    D3OutfitPreset preset;
    for (usize s = 0; s < kD3VisualSlots.size(); ++s) {
        if (const std::string* item = map.Get(SlotKey(section, s)))
            preset.items[s] = *item;
        if (const std::string* dye = map.Get(DyeKey(section, s)))
            preset.dyes[s] = std::atoi(dye->c_str());
    }
    if (const std::string* sheathed = map.Get(section + ".Sheathed"))
        preset.sheathed = *sheathed == "1";
    return preset;
}

void WriteD3OutfitPreset(const std::filesystem::path& file, std::string_view name, const D3OutfitPreset& preset) {
    ini::IniMap map;
    map.Load(file);
    const std::string section = PresetSection(name);
    map.RemovePrefix(section + ".");
    for (usize s = 0; s < kD3VisualSlots.size(); ++s) {
        if (preset.items[s])
            map.Set(SlotKey(section, s), *preset.items[s]);
        if (preset.dyes[s] != 0)
            map.Set(DyeKey(section, s), std::to_string(preset.dyes[s]));
    }
    map.Set(section + ".Sheathed", preset.sheathed.value_or(false) ? "1" : "0");
    map.Save(file);
}

} // namespace whiteout::flakes
