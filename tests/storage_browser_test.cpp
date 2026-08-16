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
#include <string>

using whiteout::flakes::ProductId;
using whiteout::flakes::io::Any;
using whiteout::flakes::io::BrowseType;
using whiteout::flakes::io::BrowseTypeLabel;
using whiteout::flakes::io::BrowseTypeOfFile;
using whiteout::flakes::io::BrowseTypesFor;
using whiteout::flakes::io::FileContentProvider;
using whiteout::flakes::io::MatchesFilter;
using whiteout::flakes::io::StorageBrowser;
using whiteout::flakes::io::StorageKind;

TEST_CASE("Each game declares what it is worth browsing for", "[browser]") {
    // Warcraft III as it always was.
    CHECK(BrowseTypesFor(ProductId::Wc3) == (BrowseType::Models | BrowseType::Effects));
    // World of Warcraft ships a world of formats this cannot draw; `.m2` is
    // the whole list.
    CHECK(BrowseTypesFor(ProductId::Wow) == BrowseType::M2);
    CHECK(BrowseTypesFor(ProductId::Sc2) == BrowseType::M3);
    // A loose folder has no product record, so it narrows nothing.
    CHECK(BrowseTypesFor(ProductId::Neutral) ==
          (BrowseType::Models | BrowseType::Effects | BrowseType::M2 | BrowseType::M3));

    // Extensions group by what a user would check, not by dialect: two model
    // dialects are one checkbox, two effect dialects are another.
    CHECK(BrowseTypeOfFile("units\\human\\peasant.MDX") == BrowseType::Models);
    CHECK(BrowseTypeOfFile("x.mdl") == BrowseType::Models);
    CHECK(BrowseTypeOfFile("x.pkb") == BrowseType::Effects);
    CHECK(BrowseTypeOfFile("x.pkfx") == BrowseType::Effects);
    CHECK(BrowseTypeOfFile("creature/cow/cow.m2") == BrowseType::M2);
    CHECK(BrowseTypeOfFile("x.m3") == BrowseType::M3);
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
