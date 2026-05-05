#include "io/replaceable_paths.h"
#include "common_types.h"
#include "io/content_provider.h"
#include "slk.h"

#include <atomic>
#include <cstdio>
#include <mutex>
#include <string>
#include <unordered_map>

namespace WhiteoutDex::io {

namespace {

std::atomic<u8> g_currentTileset{static_cast<u8>(Tileset::LordaeronSummer)};

char TilesetCode(Tileset ts) {
    switch (ts) {
        case Tileset::LordaeronSummer: return 'L';
        case Tileset::Ashenvale:       return 'A';
        case Tileset::Barrens:         return 'B';
        case Tileset::Northrend:       return 'N';
        case Tileset::Felwood:         return 'F';
        case Tileset::Dungeon:         return 'D';
        case Tileset::Cityscape:       return 'C';
        case Tileset::LordaeronFall:   return 'Q';
        case Tileset::LordaeronWinter: return 'W';
        case Tileset::Outland:         return 'O';
        case Tileset::SunkenRuins:     return 'Z';
        case Tileset::IcecrownGlacier: return 'I';
        case Tileset::DalaranRuins:    return 'X';
        case Tileset::BlackCitadel:    return 'K';
        case Tileset::Underground:     return 'G';
        case Tileset::Village:         return 'V';
        default:                       return 'L';
    }
}

struct DataCache {
    std::mutex                                  mu;
    bool                                        loaded = false;

    std::unordered_map<char, std::string>       cliffByTileset;

    std::unordered_map<i32, std::string>        pathById;
};
DataCache& Cache() { static DataCache c; return c; }

std::string JoinDirFile(std::string_view dir, std::string_view file, std::string_view ext) {
    std::string out;
    out.reserve(dir.size() + 1 + file.size() + ext.size());
    out.append(dir);
    if (!out.empty() && out.back() != '\\' && out.back() != '/')
        out += '\\';
    out.append(file);
    out.append(ext);
    return out;
}

void LogHeaders(const char*  , const SlkTable&  ) {

}

i32 FindAny(const SlkTable& t, std::initializer_list<const char*> names) {
    for (auto* n : names) { i32 c = t.FindColumn(n); if (c >= 0) return c; }
    return -1;
}

void LoadCliffTypes(IContentProvider& cp, DataCache& cache) {
    auto data = cp.ReadFile("TerrainArt\\CliffTypes.slk", nullptr);
    if (!data) {
        std::fprintf(stderr,
                     "[events] ERR: CliffTypes.slk: not found\n");
        return;
    }
    SlkTable t = ParseSlk(*data);
    LogHeaders("CliffTypes.slk", t);
    const i32 colId   = FindAny(t, {"cliffID"});
    const i32 colDir  = FindAny(t, {"texDir", "dir"});
    const i32 colFile = FindAny(t, {"texFile", "file"});
    if (colId < 0 || colDir < 0 || colFile < 0) {
        std::fprintf(stderr,
            "[WDEX replaceable] CliffTypes.slk missing expected columns "
            "(cliffID=%d texDir=%d texFile=%d). Falling back to hardcoded path.\n",
            colId, colDir, colFile);
        return;
    }
    for (usize r = 1; r < t.RowCount(); ++r) {
        std::string_view id   = t.Cell(r, colId);
        std::string_view dir  = t.Cell(r, colDir);
        std::string_view file = t.Cell(r, colFile);
        if (id.empty() || dir.empty() || file.empty()) continue;
        cache.cliffByTileset[id[0]] = JoinDirFile(dir, file, "0.blp");
    }
}

}

const char* TilesetName(Tileset ts) {
    switch (ts) {
        case Tileset::LordaeronSummer: return "Lordaeron Summer";
        case Tileset::Ashenvale:       return "Ashenvale";
        case Tileset::Barrens:         return "Barrens";
        case Tileset::Northrend:       return "Northrend";
        case Tileset::Felwood:         return "Felwood";
        case Tileset::Dungeon:         return "Dungeon";
        case Tileset::Cityscape:       return "Cityscape";
        case Tileset::LordaeronFall:   return "Lordaeron Fall";
        case Tileset::LordaeronWinter: return "Lordaeron Winter";
        case Tileset::Outland:         return "Outland";
        case Tileset::SunkenRuins:     return "Sunken Ruins";
        case Tileset::IcecrownGlacier: return "Icecrown Glacier";
        case Tileset::DalaranRuins:    return "Dalaran Ruins";
        case Tileset::BlackCitadel:    return "Black Citadel";
        case Tileset::Underground:     return "Underground";
        case Tileset::Village:         return "Village";
        default:                       return "Unknown";
    }
}

void SetCurrentTileset(Tileset ts) {
    if (static_cast<u8>(ts) >= static_cast<u8>(Tileset::Count)) return;
    g_currentTileset.store(static_cast<u8>(ts));
}

Tileset GetCurrentTileset() {
    return static_cast<Tileset>(g_currentTileset.load());
}

void LoadGameDataFiles(IContentProvider* cp, bool force) {
    if (!cp) return;
    auto& c = Cache();
    std::lock_guard<std::mutex> lk(c.mu);
    if (c.loaded && !force) return;
    c.cliffByTileset.clear();
    c.pathById.clear();
    LoadCliffTypes(*cp, c);
    c.loaded = true;
}

const char* ReplaceableCanonicalPath(i32 replaceableId, Tileset ts) {

    switch (replaceableId) {
        case 11: {
            auto& cache = Cache();
            std::lock_guard<std::mutex> lk(cache.mu);

            if (auto it = cache.pathById.find(11); it != cache.pathById.end())
                return it->second.c_str();
            const char code = TilesetCode(ts);
            if (auto it = cache.cliffByTileset.find(code); it != cache.cliffByTileset.end())
                return it->second.c_str();
            return "ReplaceableTextures\\Cliff\\Cliff0.blp";
        }

        case 21: return nullptr;

        case 31: return "ReplaceableTextures\\LordaeronTree\\LordaeronSummerTree.blp";
        case 32: return "ReplaceableTextures\\AshenvaleTree\\AshenTree.blp";
        case 33: return "ReplaceableTextures\\BarrensTree\\BarrensTree.blp";
        case 34: return "ReplaceableTextures\\NorthrendTree\\NorthTree.blp";
        case 35: return "ReplaceableTextures\\Mushroom\\MushroomTree.blp";
        case 36: return "ReplaceableTextures\\RuinsTree\\RuinsTree.blp";
        case 37: return "ReplaceableTextures\\OutlandMushroomTree\\MushroomTree.blp";

        default: return nullptr;
    }
}

const char* ReplaceableCanonicalPath(i32 replaceableId) {
    return ReplaceableCanonicalPath(replaceableId, GetCurrentTileset());
}

}
