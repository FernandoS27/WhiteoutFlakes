#include "install_locator.h"

#include "whiteout/flakes/util/path_utf8.h"

#include <cstdio>
#include <filesystem>

#if !defined(__EMSCRIPTEN__)
#include <whiteout/utils/blizzard_game_finder.h>
#endif

namespace whiteout::flakes::io {

namespace fs = std::filesystem;

InstallLocator::InstallLocator() {
#if defined(__EMSCRIPTEN__)
    // Web build: no native installs to find. The host swaps in a
    // FetchContentProvider before any request runs.
    return;
#else
    namespace BG = whiteout::utils;
    auto games = BG::findBlizzardGames();

    // One pass, four products. `Data/` presence is the tie-breaker for
    // Warcraft III only, because a Reforged install and a stale classic one
    // both report as WarcraftIII and only the former has content we can open;
    // the others have no such ambiguity.
    std::string wc3Fallback;
    for (auto& info : games) {
        switch (info.game) {
        case BG::BlizzardGame::WarcraftIII:
        case BG::BlizzardGame::WarcraftIIIReforged:
            if (wc3_.empty() && fs::exists(FsPathFromUtf8(info.path) / "Data"))
                wc3_ = info.path;
            else if (wc3Fallback.empty())
                wc3Fallback = info.path;
            break;
        // Classic and Classic Era are separate installs of the same game;
        // whichever the finder lists first wins, and a host that wants the
        // other one passes it to SetInstallPath.
        case BG::BlizzardGame::WorldOfWarcraft:
        case BG::BlizzardGame::WorldOfWarcraftClassic:
        case BG::BlizzardGame::WorldOfWarcraftClassicEra:
            if (wow_.empty())
                wow_ = info.path;
            break;
        case BG::BlizzardGame::StarCraftII:
            if (sc2_.empty())
                sc2_ = info.path;
            break;
        case BG::BlizzardGame::HeroesOfTheStorm:
            if (hots_.empty())
                hots_ = info.path;
            break;
        default:
            break;
        }
    }
    if (wc3_.empty())
        wc3_ = std::move(wc3Fallback);

    auto report = [](const char* name, const std::string& path) {
        if (!path.empty())
            std::printf("[FileContentProvider] Found %s at: %s\n", name, path.c_str());
    };
    report("Warcraft III", wc3_);
    report("World of Warcraft", wow_);
    report("StarCraft II", sc2_);
    report("Heroes of the Storm", hots_);
    if (wc3_.empty())
        std::printf("[FileContentProvider] Warcraft III installation not found.\n");
#endif
}

const std::string& InstallLocator::PathFor(ProductId game) const {
    static const std::string kEmpty;
    switch (game) {
    case ProductId::Wc3:
        return wc3_;
    case ProductId::Wow:
        return wow_;
    case ProductId::Sc2:
        return sc2_;
    default:
        return kEmpty;
    }
}

} // namespace whiteout::flakes::io
