#include "storage_explorer_ini.h"

#include "ini_file.h"
#include "settings_ini.h"

#include <filesystem>
#include <string>

namespace whiteout::flakes {

using ini::IniMap;
using ini::ParseFloat;
using ini::ParseInt;

namespace {

constexpr const char* kSection = "StorageExplorer";

std::string Key(const char* k) {
    return std::string(kSection) + "." + k;
}

// The keys, in the order the change key concatenates them.
constexpr const char* kKeys[] = {"View",   "Game",     "Types",    "Folder",
                                 "Filter", "Selected", "IconSize", "TreeSplit"};

// What the mask used to be written under, back when a browse listed images by
// default (io::DefaultEnabledTypes). Every such file records a mask WITH
// Textures in it that no user ever chose, and restoring one would put the
// images back. So the key is not read - only dropped, so the file stops
// carrying an answer to a question nobody asks any more.
constexpr const char* kLegacyTypesKey = "BrowseTypes";

// Stable strings rather than the enum values: ProductId is an ABI enum whose
// numbering is not a promise the ini should depend on. Same spellings as
// LoadIoProduct's, so a reader of the file sees one vocabulary.
const char* GameName(ProductId game) {
    switch (game) {
    case ProductId::Wow:
        return "wow";
    case ProductId::Sc2:
        return "sc2";
    case ProductId::D3:
        return "d3";
    case ProductId::Wc3:
        return "wc3";
    default:
        return ""; // Neutral: a hand-picked folder, which no game name describes
    }
}

ProductId GameFromName(const std::string& name) {
    if (name == "wow")
        return ProductId::Wow;
    if (name == "sc2")
        return ProductId::Sc2;
    if (name == "d3")
        return ProductId::D3;
    if (name == "wc3")
        return ProductId::Wc3;
    return ProductId::Neutral;
}

void WriteState(IniMap& ini, const tools::ExplorerState& st) {
    ini.Set(Key("View"), st.view == tools::ExplorerView::Tree ? "tree" : "grid");
    ini.Set(Key("Game"), GameName(st.game));
    ini.Set(Key("Types"), ini::ToString(static_cast<u32>(st.browseTypes)));
    ini.values.erase(Key(kLegacyTypesKey));
    ini.Set(Key("Folder"), st.folder);
    ini.Set(Key("Filter"), st.filter);
    ini.Set(Key("Selected"), st.selected);
    // Whole pixels: both are sizes a user drags, and three decimals of a
    // splitter position would rewrite the file for a mouse tremor.
    ini.Set(Key("IconSize"), ini::FloatToString(st.iconSize, 0));
    ini.Set(Key("TreeSplit"), ini::FloatToString(st.treeSplit, 0));
}

} // namespace

tools::ExplorerState LoadStorageExplorerState() {
    IniMap ini;
    ini.Load(SettingsIniPath());
    tools::ExplorerState st;
    if (auto* s = ini.Get(Key("View")))
        st.view = (*s == "tree") ? tools::ExplorerView::Tree : tools::ExplorerView::Grid;
    if (auto* s = ini.Get(Key("Game")))
        st.game = GameFromName(*s);
    if (auto* s = ini.Get(Key("Types"))) {
        i32 v = 0;
        // 0 stays BrowseType::None, which the panel reads as "not recorded" and
        // leaves the game's own default in place — an empty mask would restore
        // a panel that is browsing for nothing.
        if (ParseInt(*s, v) && v > 0)
            st.browseTypes = static_cast<io::BrowseType>(static_cast<u32>(v));
    }
    if (auto* s = ini.Get(Key("Folder")))
        st.folder = *s;
    if (auto* s = ini.Get(Key("Filter")))
        st.filter = *s;
    if (auto* s = ini.Get(Key("Selected")))
        st.selected = *s;
    if (auto* s = ini.Get(Key("IconSize"))) {
        f32 v = 0.0f;
        if (ParseFloat(*s, v))
            st.iconSize = v;
    }
    if (auto* s = ini.Get(Key("TreeSplit"))) {
        f32 v = 0.0f;
        if (ParseFloat(*s, v))
            st.treeSplit = v;
    }
    return st;
}

void SaveStorageExplorerState(const tools::ExplorerState& state) {
    const std::filesystem::path path = SettingsIniPath();
    // Round-trip the file so writing this section doesn't drop Display or IO.
    IniMap ini;
    ini.Load(path);
    WriteState(ini, state);
    ini.Save(path);
}

std::string ExplorerStateKey(const tools::ExplorerState& state) {
    // Built through WriteState, so a key that matches means a write that would
    // change nothing — including the rounding, which is what stops a splitter
    // drag from rewriting the file for a sub-pixel move.
    IniMap ini;
    WriteState(ini, state);
    std::string key;
    for (const char* k : kKeys) {
        if (auto* v = ini.Get(Key(k)))
            key += *v;
        key += '\n'; // a folder may contain anything else a separator could be
    }
    return key;
}

} // namespace whiteout::flakes
