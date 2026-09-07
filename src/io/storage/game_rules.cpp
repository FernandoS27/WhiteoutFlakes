#include "game_rules.h"

#include "whiteout/flakes/util/path_utf8.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <utility>

namespace whiteout::flakes::io {

namespace fs = std::filesystem;

namespace {

// Warcraft III: patch first so its overrides win, then the expansion, then the
// base game.
const char* const kWc3Archives[] = {
    "War3Patch.mpq",
    "War3x.mpq",
    "war3.mpq",
};

// Pre-Warlords World of Warcraft, for the versions whose names never moved.
// Only a starting point — see ScanArchives, which is what actually finds them.
const char* const kWowArchives[] = {
    "Data/patch-3.MPQ",   "Data/patch-2.MPQ",  "Data/patch.MPQ",  "Data/lichking.MPQ",
    "Data/expansion.MPQ", "Data/common-2.MPQ", "Data/common.MPQ",
};

std::vector<std::string> ToVector(const char* const* names, usize count) {
    std::vector<std::string> out;
    out.reserve(count);
    for (usize i = 0; i < count; ++i)
        out.emplace_back(names[i]);
    return out;
}

// ---- The four forms --------------------------------------------------------
//
// Each one says what its game has and nothing else. A reader should be able to
// tell what Warcraft III does differently from World of Warcraft by reading
// two short functions, which was the point of splitting this out.

// A TVFS mod chain plus the three War3*.mpq beside it. The only product with
// the chain, and the only one that needs the Reforged frame-suffix rename.
void ConfigureWc3(StorageBuilder& b, const StorageConfig& c, const std::atomic<bool>* hdMode) {
    if (!c.ignoreCasc)
        b.ModChain(hdMode).FrameSuffixFallback().Casc(c.installPath);
    if (!c.ignoreArchives)
        b.Archives(c.installPath, c.archives);
}

// An id-keyed root — no readable names without a community listfile, and no
// *contents* for a shipped file whose frames are TACT-encrypted without a
// community key list — plus, on pre-Warlords installs only, a Data/ archive set
// whose names move with the expansion. Retail has none, and that is not an
// error. An empty listfile path is passed through as-is: the CASC registry
// resolves it to a conventionally placed CSV (see DiscoverWowListfile in
// casc_registry.h), and it must be the one doing so — every consumer of the
// install shares a storage only while they agree on the key.
void ConfigureWow(StorageBuilder& b, const StorageConfig& c, const std::atomic<bool>*) {
    if (!c.ignoreCasc)
        b.FileIds().Listfile(c.listfilePath).TactKeys(c.tactKeyPath).Casc(c.installPath);
    if (!c.ignoreArchives)
        b.Archives(c.installPath, c.archives);
}

// CASC only — neither game ever shipped an MPQ — and two roots, because
// StarCraft II and Heroes of the Storm are separate installs sharing one
// ProductId (they share a render profile). Either may be absent. The asset
// prefixes are what let an `.m3`'s relative texture names
// ("assets/textures/...") find the mod-rooted full paths the storage stores.
void ConfigureSc2(StorageBuilder& b, const StorageConfig& c, const std::atomic<bool>*) {
    if (c.ignoreCasc)
        return;
    b.AssetPrefixes().Casc(c.installPath).Casc(c.secondaryPath);
}

// CASC only, one root, and ids that are SNO ids. The game ships its own name
// table (CoreTOC), so there is no mod chain, no asset-prefix retry, no
// community listfile and no TACT keys — the plainest of the four.
void ConfigureD3(StorageBuilder& b, const StorageConfig& c, const std::atomic<bool>*) {
    if (!c.ignoreCasc)
        b.FileIds().Casc(c.installPath);
}

} // namespace

std::unique_ptr<GameStorage> BuildGameStorage(const StorageConfig& config,
                                              const std::atomic<bool>* hdMode,
                                              ProgressMonitor* progress) {
    StorageBuilder b(config.game);
    switch (config.game) {
    case ProductId::Wow:
        ConfigureWow(b, config, hdMode);
        break;
    case ProductId::Sc2:
        ConfigureSc2(b, config, hdMode);
        break;
    case ProductId::D3:
        ConfigureD3(b, config, hdMode);
        break;
    default:
        // Neutral included: a scene with nothing loaded reads Warcraft III,
        // which is what every host that never names a product expects.
        ConfigureWc3(b, config, hdMode);
        break;
    }
    return b.Build(progress);
}

std::vector<std::string> DefaultArchives(ProductId game) {
    switch (game) {
    case ProductId::Wow:
        return ToVector(kWowArchives, std::size(kWowArchives));
    case ProductId::Sc2:
        // Deliberately empty. StarCraft II and Heroes are CASC-only — every
        // version of both — so an archive list here would be a list of files
        // that have never existed.
        return {};
    case ProductId::D3:
        // Same, for the same reason.
        return {};
    default:
        return ToVector(kWc3Archives, std::size(kWc3Archives));
    }
}

// Order is what makes an archive chain correct, and the rule is "most recently
// patched wins": the first hit is returned, so patches must precede the bases
// they overlay. Within a class, higher numbers first — patch-3 before patch-2
// before patch. Locale archives outrank their global counterparts because that
// is what they are for.
std::vector<std::string> ScanArchives(ProductId game, const std::string& installPath) {
    if (game != ProductId::Wow)
        return DefaultArchives(game);

    std::vector<std::string> out;
    if (installPath.empty())
        return out;
    const fs::path data = FsPathFromUtf8(installPath) / "Data";
    std::error_code ec;
    if (!fs::is_directory(data, ec))
        return out;

    struct Found {
        int rank;          // lower sorts first
        long long gen = 0; // trailing patch number, higher first
        std::string rel;
    };
    std::vector<Found> found;

    auto classify = [](std::string name) {
        for (auto& c : name)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        // Trailing "-N" / "N" is the patch generation.
        long long num = 0;
        for (usize i = name.size(); i-- > 0;) {
            if (std::isdigit(static_cast<unsigned char>(name[i]))) {
                long long mul = 1, v = 0;
                usize j = i;
                while (j != static_cast<usize>(-1) &&
                       std::isdigit(static_cast<unsigned char>(name[j]))) {
                    v += (name[j] - '0') * mul;
                    mul *= 10;
                    --j;
                }
                num = v;
            }
            break;
        }
        int rank = 40;
        if (name.find("wow-update") != std::string::npos)
            rank = 0;
        else if (name.find("patch") != std::string::npos)
            rank = 10;
        else if (name.find("expansion") != std::string::npos ||
                 name.find("lichking") != std::string::npos)
            rank = 20;
        else if (name.find("locale") != std::string::npos)
            rank = 30;
        return std::pair<int, long long>{rank, num};
    };

    auto sweep = [&](const fs::path& dir, int rankBias) {
        std::error_code e2;
        for (fs::directory_iterator it(dir, e2), end; it != end; it.increment(e2)) {
            if (e2)
                break;
            if (!it->is_regular_file(e2))
                continue;
            std::string ext = it->path().extension().string();
            for (auto& c : ext)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (ext != ".mpq")
                continue;
            auto [rank, num] = classify(it->path().filename().string());
            std::string rel = PathToUtf8(fs::relative(it->path(), FsPathFromUtf8(installPath), e2));
            if (rel.empty())
                continue;
            found.push_back({rank + rankBias, num, std::move(rel)});
        }
    };

    // Locale subdirectories first — a locale archive overrides the global one
    // of the same generation. Identified by holding a locale-*.mpq rather than
    // by matching a list of locale codes, so a locale nobody thought of still
    // works.
    for (fs::directory_iterator it(data, ec), end; it != end; it.increment(ec)) {
        if (ec)
            break;
        if (!it->is_directory(ec))
            continue;
        bool isLocale = false;
        std::error_code e3;
        for (fs::directory_iterator f(it->path(), e3), fend; f != fend; f.increment(e3)) {
            if (e3)
                break;
            std::string n = f->path().filename().string();
            for (auto& c : n)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (n.rfind("locale-", 0) == 0 || n.rfind("patch-", 0) == 0) {
                isLocale = true;
                break;
            }
        }
        if (isLocale)
            sweep(it->path(), -5);
    }
    sweep(data, 0);

    std::stable_sort(found.begin(), found.end(), [](const Found& a, const Found& b) {
        if (a.rank != b.rank)
            return a.rank < b.rank;
        return a.gen > b.gen;
    });
    out.reserve(found.size());
    for (auto& f : found)
        out.push_back(std::move(f.rel));
    return out;
}

} // namespace whiteout::flakes::io
