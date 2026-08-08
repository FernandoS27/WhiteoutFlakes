#include "renderer/dnc/dnc_catalog.h"

#include <algorithm>
#include <array>
#include <cctype>

namespace whiteout::flakes::renderer::dnc {

namespace {

constexpr std::string_view kLordaeron = "Lordaeron";
constexpr std::string_view kAshenvale = "Ashenvale";
constexpr std::string_view kDalaran = "Dalaran";
constexpr std::string_view kDungeon = "Dungeon";
constexpr std::string_view kFelwood = "Felwood";
constexpr std::string_view kUnderground = "Underground";

constexpr std::string_view kSdPrefix = "war3.w3mod:";
constexpr std::string_view kHdPrefix = "war3.w3mod:_hd.w3mod:";

// Enumerated from a retail install: everything under `environment\dnc\`, in
// both `war3.w3mod:` and `war3.w3mod:_hd.w3mod:`. Lordaeron leads because it
// is the engine's fallback for every unmapped tileset.
constexpr std::array<DncCatalogEntry, 14> kCatalog{{
    {kLordaeron, DncRole::Unit,
     "Environment/DNC/DNCLordaeron/DNCLordaeronUnit/DNCLordaeronUnit.mdl", true, true},
    {kLordaeron, DncRole::Terrain,
     "Environment/DNC/DNCLordaeron/DNCLordaeronTerrain/DNCLordaeronTerrain.mdl", true, true},
    {kLordaeron, DncRole::Portrait,
     "Environment/DNC/DNCLordaeron/DNCLordaeronPortrait/DNCLordaeronPortrait.mdl", true, true},
    {kLordaeron, DncRole::Target,
     "Environment/DNC/DNCLordaeron/DNCLordaeronTarget/DNCLordaeronTarget.mdl", true, false},

    {kAshenvale, DncRole::Unit,
     "Environment/DNC/DNCAshenvale/DNCAshenvaleUnit/DNCAshenvaleUnit.mdl", true, true},
    {kAshenvale, DncRole::Terrain,
     "Environment/DNC/DNCAshenvale/DNCAshenvaleTerrain/DNCAshenvaleTerrain.mdl", true, true},

    {kDalaran, DncRole::Unit, "Environment/DNC/DNCDalaran/DNCDalaranUnit/DNCDalaranUnit.mdl", true,
     true},
    {kDalaran, DncRole::Terrain,
     "Environment/DNC/DNCDalaran/DNCDalaranTerrain/DNCDalaranTerrain.mdl", true, true},

    {kDungeon, DncRole::Unit, "Environment/DNC/DNCDungeon/DNCDungeonUnit/DNCDungeonUnit.mdl", true,
     true},
    {kDungeon, DncRole::Terrain,
     "Environment/DNC/DNCDungeon/DNCDungeonTerrain/DNCDungeonTerrain.mdl", true, true},

    {kFelwood, DncRole::Unit, "Environment/DNC/DNCFelwood/DNCFelwoodUnit/DNCFelwoodUnit.mdl", true,
     true},
    {kFelwood, DncRole::Terrain,
     "Environment/DNC/DNCFelwood/DNCFelwoodTerrain/DNCFelwoodTerrain.mdl", true, true},

    {kUnderground, DncRole::Unit,
     "Environment/DNC/DNCUnderground/DNCUndergroundUnit/DNCUndergroundUnit.mdl", true, true},
    {kUnderground, DncRole::Terrain,
     "Environment/DNC/DNCUnderground/DNCUndergroundTerrain/DNCUndergroundTerrain.mdl", true, true},
}};

// `UI\WorldEditData.txt` [TileSets], with the family both [UnitLights] and
// [TerrainLights] map each code to — the two tables agree everywhere, so one
// column covers both. Names come from WESTRING_LOCALE_* in enUS
// WorldEditGameStrings.txt.
constexpr std::array<DncTilesetEntry, 18> kTilesets{{
    {'A', "Ashenvale", kAshenvale},
    {'B', "Barrens", kLordaeron},
    {'K', "Black Citadel", kLordaeron},
    {'Y', "Cityscape", kLordaeron},
    {'X', "Dalaran", kDalaran},
    {'J', "Dalaran Ruins", kDalaran},
    {'D', "Dungeon", kDungeon},
    {'C', "Felwood", kFelwood},
    {'I', "Icecrown Glacier", kLordaeron},
    {'F', "Lordaeron Fall", kLordaeron},
    {'L', "Lordaeron Summer", kLordaeron},
    {'W', "Lordaeron Winter", kLordaeron},
    {'N', "Northrend", kLordaeron},
    {'O', "Outland", kLordaeron},
    {'Z', "Sunken Ruins", kLordaeron},
    {'G', "Underground", kUnderground},
    {'V', "Village", kLordaeron},
    {'Q', "Village Fall", kLordaeron},
}};

// `[DayEnvironmentMap]` / `[NightEnvironmentMap]`. Shipped WorldEditData has
// every row but `D` and `L` commented out, so all the other tilesets take the
// `Default` line — Lordaeron Summer. Dungeon is the one tileset with its own
// probe, and it names the *night* map for both phases. Paths keep the table's
// `.tif` spelling; the content provider's texture-extension fallback resolves
// them to the shipped `.dds`.
constexpr std::string_view kEnvMapDayDefault =
    "Environment/EnvironmentMap/LordaeronSummer/Day_IBL.tif";
constexpr std::string_view kEnvMapNightDefault =
    "Environment/EnvironmentMap/LordaeronSummer/Night_IBL.tif";
constexpr std::string_view kEnvMapDungeon = "Environment/EnvironmentMap/Dungeon/Night_IBL.tif";
constexpr std::string_view kEnvMapPortrait =
    "Environment/EnvironmentMap/Portraits/PortraitDefault_IBL.tif";

std::string_view RoleName(DncRole r) {
    switch (r) {
    case DncRole::Unit:
        return "Unit";
    case DncRole::Terrain:
        return "Terrain";
    case DncRole::Portrait:
        return "Portrait";
    case DncRole::Target:
        return "Target";
    }
    return "";
}

const DncCatalogEntry* Find(std::string_view family, DncRole role) {
    for (const auto& e : kCatalog)
        if (e.family == family && e.role == role)
            return &e;
    return nullptr;
}

// Strip any `…w3mod:` chain and the extension, then compare case- and
// separator-insensitively — a pinned path, or one naming the `.mdx` CASC
// actually stores, still matches its catalog row.
std::string Canonical(std::string_view path) {
    if (const auto colon = path.rfind(':'); colon != std::string_view::npos)
        path.remove_prefix(colon + 1);
    if (const auto dot = path.rfind('.'); dot != std::string_view::npos)
        path.remove_suffix(path.size() - dot);
    std::string out;
    out.reserve(path.size());
    for (char c : path) {
        if (c == '\\')
            c = '/';
        out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

} // namespace

std::span<const DncCatalogEntry> DncCatalog() {
    return kCatalog;
}

std::span<const DncTilesetEntry> DncTilesets() {
    return kTilesets;
}

std::string_view DncPathForTileset(char tilesetCode, DncRole role) {
    const char code = static_cast<char>(std::toupper(static_cast<unsigned char>(tilesetCode)));
    for (const auto& ts : kTilesets)
        if (ts.code == code)
            if (const auto* e = Find(ts.family, role))
                return e->path;
    // WEUDataGetTileSetLight's fallback for codes with no table entry.
    const auto* fallback = Find(kLordaeron, role);
    return fallback ? fallback->path : std::string_view{};
}

std::string_view EnvMapForTileset(char tilesetCode, EnvMapPhase phase) {
    if (std::toupper(static_cast<unsigned char>(tilesetCode)) == 'D')
        return kEnvMapDungeon;
    return phase == EnvMapPhase::Night ? kEnvMapNightDefault : kEnvMapDayDefault;
}

std::string_view PortraitEnvMap() {
    return kEnvMapPortrait;
}

std::string DncPathForVariant(std::string_view path, DncVariant variant) {
    if (const auto colon = path.rfind(':'); colon != std::string_view::npos)
        path.remove_prefix(colon + 1);
    switch (variant) {
    case DncVariant::Sd:
        return std::string(kSdPrefix) + std::string(path);
    case DncVariant::Hd:
        return std::string(kHdPrefix) + std::string(path);
    case DncVariant::Auto:
        break;
    }
    return std::string(path);
}

i32 DncCatalogIndexOf(std::string_view path) {
    const std::string want = Canonical(path);
    for (usize i = 0; i < kCatalog.size(); ++i)
        if (Canonical(kCatalog[i].path) == want)
            return static_cast<i32>(i);
    return -1;
}

DncVariant DncVariantOf(std::string_view path) {
    if (path.starts_with(kHdPrefix))
        return DncVariant::Hd;
    if (path.starts_with(kSdPrefix))
        return DncVariant::Sd;
    return DncVariant::Auto;
}

std::string DncEntryLabel(const DncCatalogEntry& e) {
    return std::string(e.family) + ' ' + std::string(RoleName(e.role));
}

} // namespace whiteout::flakes::renderer::dnc
