// ============================================================================
// What a storage is browsed for, and by whom.
//
// The browser used to admit exactly four extensions, which meant a World of
// Warcraft install browsed to an empty tree and a `.m2` was unreachable from
// the explorer at all. Now the set is the open *game's* — and since that set is
// also what a host turns into checkboxes, the two have to agree: a filter for a
// type the storage was never walked for would be a control that does nothing.
//
// The install-backed cases skip without one; skipped is not passed.
// ============================================================================

#include <catch2/catch_test_macros.hpp>

#include "io/file_content_provider.h"
#include "io/storage_browser.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

using whiteout::flakes::ProductId;
using whiteout::flakes::io::Any;
using whiteout::flakes::io::BrowseType;
using whiteout::flakes::io::BrowseTypeLabel;
using whiteout::flakes::io::BrowseTypeOfFile;
using whiteout::flakes::io::BrowseTypesFor;
using whiteout::flakes::io::FileContentProvider;
using whiteout::flakes::io::MatchesFilter;
using whiteout::flakes::io::OpenWithArchives;
using whiteout::flakes::io::StorageBrowser;
using whiteout::flakes::io::StorageKind;

TEST_CASE("Each game declares what it is worth browsing for", "[browser]") {
    // Warcraft III as it always was.
    CHECK(BrowseTypesFor(ProductId::Wc3) == (BrowseType::Models | BrowseType::Effects));
    // World of Warcraft ships a world of formats this cannot draw; `.m2` is
    // the whole list.
    CHECK(BrowseTypesFor(ProductId::Wow) == BrowseType::M2);
    CHECK(BrowseTypesFor(ProductId::Sc2) == BrowseType::M3);
    CHECK(BrowseTypesFor(ProductId::D3) == BrowseType::Actor);
    // A loose folder has no product record, so it narrows nothing.
    CHECK(BrowseTypesFor(ProductId::Neutral) ==
          (BrowseType::Models | BrowseType::Effects | BrowseType::M2 | BrowseType::M3 |
           BrowseType::Actor));

    // Extensions group by what a user would check, not by dialect: two model
    // dialects are one checkbox, two effect dialects are another.
    CHECK(BrowseTypeOfFile("units\\human\\peasant.MDX") == BrowseType::Models);
    CHECK(BrowseTypeOfFile("x.mdl") == BrowseType::Models);
    CHECK(BrowseTypeOfFile("x.pkb") == BrowseType::Effects);
    CHECK(BrowseTypeOfFile("x.pkfx") == BrowseType::Effects);
    CHECK(BrowseTypeOfFile("creature/cow/cow.m2") == BrowseType::M2);
    CHECK(BrowseTypeOfFile("x.m3") == BrowseType::M3);
    // Both Diablo III entry points are one checkbox: an `.acr` is the actor a
    // user names and an `.app` is the model it resolves to, and browsing for
    // one without the other would hide half the corpus.
    CHECK(BrowseTypeOfFile("x.acr") == BrowseType::Actor);
    CHECK(BrowseTypeOfFile("x.app") == BrowseType::Actor);
    // Not everything in an archive is a model.
    CHECK(BrowseTypeOfFile("x.blp") == BrowseType::None);
    CHECK(BrowseTypeOfFile("noextension") == BrowseType::None);
    // `.m2` must not be caught by a suffix test that only looks for "m2".
    CHECK(BrowseTypeOfFile("x.m2a") == BrowseType::None);

    // Every offerable bit has a label; the empty mask and combinations do not.
    for (BrowseType t : {BrowseType::Models, BrowseType::Effects, BrowseType::M2, BrowseType::M3})
        CHECK(std::string(BrowseTypeLabel(t)).size() > 0);
    CHECK(std::string(BrowseTypeLabel(BrowseType::None)).empty());
}

TEST_CASE("The filter box takes the patterns a filter box is expected to take", "[browser]") {
    // Nothing typed matches everything — the box is not a gate.
    CHECK(MatchesFilter("peasant.mdx", ""));
    CHECK(MatchesFilter("peasant.mdx", "   "));

    // Substring, case-insensitive both ways.
    CHECK(MatchesFilter("units\\Human\\Peasant.mdx", "peas"));
    CHECK(MatchesFilter("PEASANT.MDX", "peasant"));
    CHECK_FALSE(MatchesFilter("footman.mdx", "peasant"));
    // A term may contain spaces: only commas separate.
    CHECK(MatchesFilter("night elf archer.mdx", "night elf"));

    // A term with a wildcard is matched against the WHOLE name, so `*.mdx` is
    // an extension test rather than "contains .mdx".
    CHECK(MatchesFilter("peasant.mdx", "*.mdx"));
    CHECK_FALSE(MatchesFilter("peasant.mdx.bak", "*.mdx"));
    CHECK(MatchesFilter("footman.mdx", "foot?an*"));
    CHECK_FALSE(MatchesFilter("peasant.mdx", "*.pkb"));
    // Stars alone, and a trailing run of them, still terminate.
    CHECK(MatchesFilter("peasant.mdx", "***"));
    CHECK(MatchesFilter("peasant.mdx", "peas***"));

    // Commas are alternatives.
    CHECK(MatchesFilter("footman.mdx", "peasant, footman"));
    CHECK(MatchesFilter("peasant.mdx", "peasant, footman"));
    CHECK_FALSE(MatchesFilter("rifleman.mdx", "peasant, footman"));
    // Empty terms are ignored rather than matching nothing.
    CHECK(MatchesFilter("footman.mdx", "footman,,"));

    // A '-' term excludes, and beats every include.
    CHECK(MatchesFilter("peasant.mdx", "-portrait"));
    CHECK_FALSE(MatchesFilter("peasant_portrait.mdx", "-portrait"));
    CHECK(MatchesFilter("peasant.mdx", "peasant, -portrait"));
    CHECK_FALSE(MatchesFilter("peasant_portrait.mdx", "peasant, -portrait"));
    CHECK_FALSE(MatchesFilter("peasant_portrait.mdx", "peasant, -*portrait*"));
}

TEST_CASE("A Warcraft III browse offers models and effects, and honours both",
          "[browser][casc]") {
    FileContentProvider probe;
    if (probe.Wc3Path().empty())
        SKIP("no Warcraft III install found");

    StorageBrowser br;
    std::string error;
    REQUIRE(br.Open(probe.Wc3Path(), StorageKind::Casc, &error));
    CHECK(br.Product() == ProductId::Wc3);
    CHECK(br.AvailableTypes() == (BrowseType::Models | BrowseType::Effects));
    // Everything the game has, until a host says otherwise.
    CHECK(br.EnabledTypes() == br.AvailableTypes());

    // Found rather than named: which folders a build ships is a property of the
    // install, and a hardcoded one rots on the next patch.
    for (int depth = 0; depth < 8 && br.Current().modelFiles.empty(); ++depth) {
        if (br.Current().folders.empty())
            break;
        br.Descend(br.Current().folders.front());
    }
    INFO(br.CurrentPath());
    REQUIRE_FALSE(br.Current().modelFiles.empty());

    auto countAt = [&](BrowseType types) {
        br.SetEnabledTypes(types);
        return br.Current().modelFiles.size();
    };
    const std::size_t all = countAt(br.AvailableTypes());
    const std::size_t models = countAt(BrowseType::Models);
    const std::size_t effects = countAt(BrowseType::Effects);
    std::printf("[browser] wc3 %s: %zu all, %zu models, %zu effects\n", br.CurrentPath().c_str(),
                all, models, effects);
    // The two halves partition the whole: nothing counted twice, nothing lost.
    CHECK(models + effects == all);
    CHECK(countAt(BrowseType::None) == 0);

    // ...and what is listed is only ever what is enabled.
    br.SetEnabledTypes(BrowseType::Models);
    for (const auto& f : br.Current().modelFiles) {
        INFO(f);
        CHECK(BrowseTypeOfFile(f) == BrowseType::Models);
    }

    // The text filter narrows the same listing, and the type filter stays where
    // it was: they are two independent controls over one folder.
    br.SetEnabledTypes(br.AvailableTypes());
    const std::size_t total = br.Current().modelFiles.size();
    CHECK(br.Current().fileTotal == total); // no filter: everything is shown

    auto lower = [](std::string s) {
        std::transform(s.begin(), s.end(), s.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return s;
    };
    const std::string first = br.Current().modelFiles.front();
    const std::string ext = first.substr(first.find_last_of('.'));
    br.SetFilter("*" + ext);
    REQUIRE_FALSE(br.Current().modelFiles.empty());
    for (const auto& f : br.Current().modelFiles) {
        INFO(f);
        REQUIRE(f.size() >= ext.size());
        CHECK(lower(f).rfind(lower(ext)) == f.size() - ext.size());
    }
    // The total is what the folder holds, not what survived the filter — it is
    // the denominator a host shows as "n of m".
    CHECK(br.Current().fileTotal == total);
    CHECK(br.EnabledTypes() == br.AvailableTypes());

    br.SetFilter("no-such-thing-anywhere");
    CHECK(br.Current().modelFiles.empty());
    CHECK(br.Current().folders.empty()); // folders are filtered by the same pattern
    CHECK(br.Current().fileTotal == total);
    br.SetFilter("");
    CHECK(br.Current().modelFiles.size() == total);

    // Asking for a type this storage was not walked for cannot turn one on.
    br.SetEnabledTypes(BrowseType::M2);
    CHECK(br.EnabledTypes() == BrowseType::None);
}

TEST_CASE("A World of Warcraft browse is .m2 and nothing else", "[browser][casc]") {
    FileContentProvider probe;
    if (probe.GamePath(ProductId::Wow).empty())
        SKIP("no World of Warcraft install found");
    // The listfile is what makes an id-keyed root browsable at all, so without
    // one there is nothing here to check — see FileContentProvider. The corpus
    // ships one; fall back to it so this runs on a machine that has never
    // opened the settings panel.
    std::string listfile = probe.ListfilePath();
    if (listfile.empty()) {
        const char* env = std::getenv("WDX_TEST_LISTFILE");
        listfile = env ? env : "C:/Projects/WhiteoutLib/Corpus/community-listfile.csv";
        if (!std::filesystem::exists(listfile))
            SKIP("no listfile; a WoW root carries no names without one");
    }

    StorageBrowser br;
    br.SetCascKeys(listfile, probe.TactKeyPath());
    std::string error;
    REQUIRE(br.Open(probe.GamePath(ProductId::Wow), StorageKind::Casc, &error));
    CHECK(br.Product() == ProductId::Wow);
    CHECK(br.AvailableTypes() == BrowseType::M2);

    // `creature/` is a directory of directories, so descend until there are
    // files: an empty listing would pass the loop below without proving a
    // thing.
    br.NavigateTo("creature");
    REQUIRE_FALSE(br.Current().folders.empty());
    for (int depth = 0; depth < 4 && br.Current().modelFiles.empty(); ++depth) {
        if (br.Current().folders.empty())
            break;
        br.Descend(br.Current().folders.front());
    }
    INFO(br.CurrentPath());
    REQUIRE_FALSE(br.Current().modelFiles.empty());
    std::printf("[browser] wow %s: %zu files\n", br.CurrentPath().c_str(),
                br.Current().modelFiles.size());

    // The point of the whole exercise: a WoW browse shows `.m2` and the `.blp`,
    // `.skin`, `.anim` and `.phys` siblings beside each one do not appear.
    for (const auto& f : br.Current().modelFiles) {
        INFO(f);
        CHECK(BrowseTypeOfFile(f) == BrowseType::M2);
    }

    // ...and what it lists is readable through a provider configured the way
    // StorageExplorer::OpenCasc configures its own — which is the part that was
    // missing. A scene detecting `.m2` sets its own product, but SceneView
    // deliberately does not push that onto an *external* provider (it is shared
    // with every other scene), so nothing else was ever going to say "this
    // provider serves World of Warcraft" and every thumbnail read missed.
    FileContentProvider reader;
    reader.SetGame(ProductId::Wow);
    reader.SetListfilePath(listfile);
    reader.SetInstallPath(br.Root());

    const std::string archivePath = br.ChildPath(br.Current().modelFiles.front());
    INFO(archivePath);
    REQUIRE_FALSE(archivePath.empty());
    auto bytes = reader.ReadFile(archivePath);
    REQUIRE(bytes);
    // 'MD21' is the chunked M2 magic; anything else means a wrong file, not a
    // wrong game.
    REQUIRE(bytes->size() > 4);
    CHECK(std::string(reinterpret_cast<const char*>(bytes->data()), 4) == "MD21");
}

// The same walk for StarCraft II / Heroes, which had none. The two roots differ
// in the way that matters here: WoW's is id-keyed and unbrowsable without a
// listfile, SC2's carries its own names — so this asks for no keys at all, and
// a browse that comes back empty is a real failure rather than a missing file.
TEST_CASE("A StarCraft II browse is .m3 and nothing else", "[browser][casc]") {
    FileContentProvider probe;
    // Either install serves: they share ProductId::Sc2 and ship the same format
    // through the same frame, which is what the profile is named for.
    std::string root = probe.GamePath(ProductId::Sc2);
    if (root.empty())
        root = probe.HotsPath();
    if (root.empty())
        SKIP("no StarCraft II or Heroes of the Storm install found");

    StorageBrowser br;
    std::string error;
    REQUIRE(br.Open(root, StorageKind::Casc, &error));
    CHECK(br.Product() == ProductId::Sc2);
    CHECK(br.AvailableTypes() == BrowseType::M3);

    // No fixed entry point, unlike WoW's `creature/`: SC2 buries its assets
    // under a mod/base chain whose names moved between versions. Descend the
    // first branch until files appear — an empty listing would pass a per-file
    // loop without checking anything.
    for (int depth = 0; depth < 10 && br.Current().modelFiles.empty(); ++depth) {
        if (br.Current().folders.empty())
            break;
        br.Descend(br.Current().folders.front());
    }
    INFO(br.CurrentPath());
    REQUIRE_FALSE(br.Current().modelFiles.empty());
    std::printf("[browser] sc2 %s: %zu files\n", br.CurrentPath().c_str(),
                br.Current().modelFiles.size());

    for (const auto& f : br.Current().modelFiles) {
        INFO(f);
        CHECK(BrowseTypeOfFile(f) == BrowseType::M3);
    }

    // And readable through a provider configured the way StorageExplorer
    // configures its own, which is what turns a listed name into a loaded model.
    FileContentProvider reader;
    reader.SetGame(ProductId::Sc2);
    reader.SetInstallPath(br.Root());

    const std::string archivePath = br.ChildPath(br.Current().modelFiles.front());
    INFO(archivePath);
    REQUIRE_FALSE(archivePath.empty());
    auto bytes = reader.ReadFile(archivePath);
    REQUIRE(bytes);
    REQUIRE(bytes->size() > 4);
    // "43DM" / "33DM" — MD34 (release) and MD33 (beta) written little-endian.
    // The same two the loader's LooksLikeM3 admits, spelled the same way.
    const std::string magic(reinterpret_cast<const char*>(bytes->data()), 4);
    CHECK((magic == "43DM" || magic == "33DM"));
}

// ============================================================================
// The tree view's filter is a different filter.
//
// The grid narrows ONE folder by entry name. An outline shows every level at
// once, where that rule collapses: `units` does not contain the string
// "footman", so a name filter hides the only route to the file that does. The
// tree matches full paths instead and keeps a folder exactly when something
// under it survived, which prunes subtrees rather than levels.
//
// Folder-backed so it is deterministic and needs no install: the tree the
// browser builds is the same one either way.
// ============================================================================
namespace {

// A tiny model tree on disk, removed when the test leaves.
struct TempTree {
    std::filesystem::path root;
    explicit TempTree(const char* tag) {
        root = std::filesystem::temp_directory_path() / (std::string("wf_browser_") + tag);
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
        for (const char* rel : {"units/human/footman/footman.mdx",
                                "units/human/footman/footman_portrait.mdx",
                                "units/orc/grunt/grunt.mdx", "effects/blood/humanblood.mdx"}) {
            const std::filesystem::path p = root / rel;
            std::filesystem::create_directories(p.parent_path(), ec);
            std::FILE* f = std::fopen(p.string().c_str(), "wb");
            if (f)
                std::fclose(f);
        }
    }
    ~TempTree() {
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }
};

std::vector<std::string> Sorted(std::vector<std::string> v) {
    std::sort(v.begin(), v.end());
    return v;
}

} // namespace

TEST_CASE("The tree filter prunes subtrees, where the grid filter prunes levels", "[browser]") {
    TempTree tmp("tree_filter");
    StorageBrowser br;
    std::string err;
    REQUIRE(br.Open(tmp.root.string(), StorageKind::Folder, &err));

    // Unfiltered, the outline is simply the tree.
    CHECK(Sorted(br.TreeChildren("").folders) == std::vector<std::string>{"effects", "units"});
    CHECK(br.TreeChildren("").files.empty());
    CHECK(Sorted(br.TreeChildren("units").folders) == std::vector<std::string>{"human", "orc"});
    CHECK(Sorted(br.TreeChildren("units\\human\\footman").files) ==
          std::vector<std::string>{"footman.mdx", "footman_portrait.mdx"});
    // Nothing was pruned, so there is nothing to count.
    CHECK(br.TreeMatchCount() == 0);

    br.SetFilter("footman");

    // The grid's answer, and the reason this API exists: at the root, no ENTRY
    // is named "footman", so the folder view of the same filter is empty.
    br.NavigateTo("");
    CHECK(br.Current().folders.empty());

    // The tree's answer: the one route to the match survives, whole.
    CHECK(br.TreeChildren("").folders == std::vector<std::string>{"units"});
    CHECK(br.TreeChildren("units").folders == std::vector<std::string>{"human"});
    CHECK(br.TreeChildren("units\\human").folders == std::vector<std::string>{"footman"});
    CHECK(Sorted(br.TreeChildren("units\\human\\footman").files) ==
          std::vector<std::string>{"footman.mdx", "footman_portrait.mdx"});
    // Three folders lead to the two matches; `effects` and `orc` do not.
    CHECK(br.TreeVisibleFolderCount() == 3);
    CHECK(br.TreeMatchCount() == 2);

    // A filter that hits the other branch prunes the other way round, and the
    // level they share keeps only the child that leads somewhere.
    br.SetFilter("grunt");
    CHECK(br.TreeChildren("").folders == std::vector<std::string>{"units"});
    CHECK(br.TreeChildren("units").folders == std::vector<std::string>{"orc"});
    CHECK(br.TreeMatchCount() == 1);

    // A folder's own name counts too, because it is part of the path its files
    // are matched by: everything under `effects` survives "effects".
    br.SetFilter("effects");
    CHECK(br.TreeChildren("").folders == std::vector<std::string>{"effects"});
    CHECK(br.TreeChildren("effects\\blood").files ==
          std::vector<std::string>{"humanblood.mdx"});

    // An exclusion still beats every include, applied to the whole path.
    br.SetFilter("footman, -portrait");
    CHECK(br.TreeChildren("units\\human\\footman").files ==
          std::vector<std::string>{"footman.mdx"});
    CHECK(br.TreeMatchCount() == 1);

    // Nothing matches: the tree empties rather than falling back to everything.
    br.SetFilter("nosuchmodel");
    CHECK(br.TreeChildren("").folders.empty());
    CHECK(br.TreeMatchCount() == 0);

    // And clearing it restores the whole tree - the cache is keyed to the
    // filter, not baked at open.
    br.SetFilter("");
    CHECK(Sorted(br.TreeChildren("").folders) == std::vector<std::string>{"effects", "units"});
}

TEST_CASE("The tree reads any folder, without moving the one the grid is in", "[browser]") {
    TempTree tmp("tree_paths");
    StorageBrowser br;
    std::string err;
    REQUIRE(br.Open(tmp.root.string(), StorageKind::Folder, &err));

    // ChildPathAt is ChildPath for a folder that is not the current one - what
    // an outline needs, since every row it draws is somewhere else.
    br.NavigateTo("");
    const std::string archive = br.ChildPathAt("units\\orc\\grunt", "grunt.mdx");
    CHECK_FALSE(archive.empty());
    CHECK(std::filesystem::exists(archive));
    CHECK(br.CurrentPath().empty()); // and reading it moved nothing
    CHECK(br.ChildPath("grunt.mdx").empty());

    // A path that is not there answers empty rather than throwing.
    CHECK(br.ChildPathAt("units\\orc\\nosuchfolder", "grunt.mdx").empty());
    CHECK(br.TreeChildren("units\\orc\\nosuchfolder").folders.empty());

    // The type mask applies to the tree as well: with models off, the folders
    // that led only to models stop leading anywhere.
    br.SetEnabledTypes(BrowseType::Effects);
    CHECK(br.TreeChildren("units\\orc\\grunt").files.empty());
    br.SetFilter("grunt");
    CHECK(br.TreeChildren("").folders.empty());
    CHECK(br.TreeMatchCount() == 0);
}

// ============================================================================
// One file, one row.
//
// A host may lay archives over a Reforged install — that is what the additive
// half of OpenWithArchives is for — and the obvious thing to lay over it is the
// classic game's own War3*.mpq. Then the same model is named twice: the install
// spells it through the mod chain and in lower case
// ("war3.w3mod:units\human\footman\footman.mdx"), the archive spells it however
// its author typed it ("Units\Human\Footman\Footman.mdx").
//
// Folders were keyed case-insensitively and files were not, so those two landed
// in one folder as two rows. Worse, the archive's spelling carries no mod chain,
// and a bare Warcraft III path resolves through the chain — HD first, as the
// reader had it — so the second "footman.mdx" previewed and imported as the HD
// model. Hence the report: two footmen in units\human\footman, and an HD model
// imported from an SD path.
//
// Needs both generations installed, which is the configuration that shows it.
// ============================================================================
namespace {

// The classic Warcraft III archives beside @p wc3Path, in load order. Empty
// when this machine has only the Reforged install. Found by walking the
// siblings of the Reforged directory for one that holds War3.mpq: a downgraded
// install lives next to the one it was downgraded from, and its folder name is
// not fixed.
std::vector<std::string> ClassicArchivesNear(const std::string& wc3Path) {
    static const char* kOrder[] = {"War3xLocal.mpq", "War3Local.mpq", "War3x.mpq", "War3.mpq",
                                   "Deprecated.mpq"};
    std::error_code ec;
    const std::filesystem::path parent = std::filesystem::path(wc3Path).parent_path();
    for (std::filesystem::directory_iterator it(parent, ec), end; it != end; it.increment(ec)) {
        if (ec)
            break;
        if (!it->is_directory(ec))
            continue;
        if (!std::filesystem::exists(it->path() / "War3.mpq", ec))
            continue;
        std::vector<std::string> out;
        for (const char* name : kOrder) {
            const std::filesystem::path p = it->path() / name;
            if (std::filesystem::is_regular_file(p, ec))
                out.push_back(p.string());
        }
        if (!out.empty())
            return out;
    }
    return {};
}

} // namespace

TEST_CASE("A Warcraft III folder lists each model once, by its mod-chain name",
          "[browser][casc]") {
    FileContentProvider probe;
    if (probe.Wc3Path().empty())
        SKIP("no Warcraft III install found");
    const std::vector<std::string> archives = ClassicArchivesNear(probe.Wc3Path());
    if (archives.empty())
        SKIP("no classic Warcraft III archives to lay over the install");

    StorageBrowser br;
    std::string error;
    // Exactly what a host with a configured archive list opens: the install,
    // then its archives on top.
    REQUIRE(OpenWithArchives(br, probe.Wc3Path(), StorageKind::Casc, archives, &error));
    REQUIRE(br.Product() == ProductId::Wc3);

    auto lower = [](std::string s) {
        for (char& c : s)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    };

    // No folder anywhere may hold two spellings of one name. Checked over the
    // whole tree rather than one folder: the duplication is a property of how
    // the two namespaces overlap, so a fix that only tidied `units` would not
    // be one.
    std::size_t files = 0;
    std::vector<std::string> dupes;
    std::function<void(const std::string&)> walk = [&](const std::string& path) {
        const auto here = br.TreeChildren(path);
        std::vector<std::string> keys;
        for (const std::string& f : here.files) {
            ++files;
            keys.push_back(lower(f));
        }
        std::sort(keys.begin(), keys.end());
        for (std::size_t i = 1; i < keys.size(); ++i)
            if (keys[i] == keys[i - 1] && dupes.size() < 10)
                dupes.push_back(path + "\\" + keys[i]);
        for (const std::string& d : here.folders)
            walk(path.empty() ? d : path + "\\" + d);
    };
    walk("");
    INFO("first duplicates: " << (dupes.empty() ? std::string("-") : dupes.front()));
    CHECK(dupes.empty());
    CHECK(files > 1000); // the walk actually walked

    // And the surviving spelling is the one that says which mod it came from.
    // A stock unit, so this is about the rule and not about one odd asset.
    const auto sd = br.TreeChildren("units\\human\\footman");
    REQUIRE_FALSE(sd.files.empty());
    CHECK(sd.files.size() == 2); // footman.mdx and footman_portrait.mdx
    const std::string archive = lower(br.ChildPathAt("units\\human\\footman", "footman.mdx"));
    CHECK(archive.rfind("war3.w3mod:", 0) == 0);
    CHECK(archive.find("_hd.w3mod") == std::string::npos);

    // The HD model is still there — under the mod folder it belongs to.
    const std::string hd =
        lower(br.ChildPathAt("_hd.w3mod\\units\\human\\footman", "footman.mdx"));
    CHECK(hd.find("_hd.w3mod:") != std::string::npos);
    CHECK(hd != archive);

    // A name the caller spells differently still resolves: the lookup is keyed
    // the same way the tree is.
    CHECK(br.ChildPathAt("units\\human\\footman", "FOOTMAN.MDX") ==
          br.ChildPathAt("units\\human\\footman", "footman.mdx"));
}
