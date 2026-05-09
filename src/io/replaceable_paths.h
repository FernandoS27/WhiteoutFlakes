#pragma once

#include "common_types.h"

namespace whiteout::flakes::io {
class IContentProvider;
}

namespace whiteout::flakes::io {

enum class Tileset : u8 {
    LordaeronSummer = 0,
    Ashenvale,
    Barrens,
    Northrend,
    Felwood,
    Dungeon,
    Cityscape,
    LordaeronFall,
    LordaeronWinter,
    Outland,
    SunkenRuins,
    IcecrownGlacier,
    DalaranRuins,
    BlackCitadel,
    Underground,
    Village,
    Count,
};

const char* TilesetName(Tileset ts);

void     SetCurrentTileset(Tileset ts);
Tileset  GetCurrentTileset();

const char* ReplaceableCanonicalPath(i32 replaceableId, Tileset ts);

const char* ReplaceableCanonicalPath(i32 replaceableId);

void LoadGameDataFiles(IContentProvider* cp, bool force = false);

}
