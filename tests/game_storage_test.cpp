// ============================================================================
// The per-game storage layer: path spelling, the archive rules each product
// follows, and what the builder assembles from them.
//
// These were unreachable while they lived as file-static helpers inside
// FileContentProvider — the naming logic every single read depends on had no
// direct test, only the render gates downstream of it. Splitting the class up
// is what made them testable, so this is the test that split is for.
//
// Device-free and install-free: nothing here opens a real storage.
// ============================================================================

#include <catch2/catch_test_macros.hpp>

#include "io/storage/game_rules.h"
#include "io/storage/storage_paths.h"

#include <string>

using namespace whiteout::flakes::io;
using whiteout::flakes::ProductId;

TEST_CASE("Paths are spelled the way each backend stores them", "[storage]") {
    // CASC: lowercase, backslashes, no leading separator.
    CHECK(NormalizeCascPath("Textures/Water07-0.BLP") == "textures\\water07-0.blp");
    CHECK(NormalizeCascPath("\\\\Units\\Human\\Peasant.mdx") == "units\\human\\peasant.mdx");

    // Listing form drops the CASC mod chain, so a listed path can be handed
    // straight back to a read — the source re-applies its own prefixes.
    CHECK(ToListingPath("war3.w3mod:_hd.w3mod:units\\x.mdx") == "units/x.mdx");
    CHECK(ToListingPath("/Units/X.MDX") == "units/x.mdx");

    // A directory filter compares in that same form.
    CHECK(NormalizeListingDir("Textures\\FX\\") == "textures/fx");
    CHECK(MatchesListingDir("textures/fx/a.blp", "textures/fx", false));
    CHECK(MatchesListingDir("textures/fx/sub/a.blp", "textures/fx", true));
    // Non-recursive must not reach into a subdirectory...
    CHECK_FALSE(MatchesListingDir("textures/fx/sub/a.blp", "textures/fx", false));
    // ...and a prefix that is not a whole path component is not a match.
    CHECK_FALSE(MatchesListingDir("textures/fxsub/a.blp", "textures/fx", true));
    // Empty dir means "everything".
    CHECK(MatchesListingDir("a.blp", "", false));
}

TEST_CASE("Extension alternatives are what a Reforged install needs", "[storage]") {
    // A classic model asks for .blp and a Reforged install answers with .dds;
    // .mdl and .mdx shadow each other the same way. Anything else has no
    // alternatives, and probing for some would be wasted archive lookups.
    CHECK(GetLowerExtension("Water07.BLP") == ".blp");
    CHECK(GetLowerExtension("noext").empty());
    CHECK(StripExtension("units/x.mdx") == "units/x");
    CHECK(StripExtension("noext") == "noext");

    auto [tex, texCount] = AltExtensionsFor(".blp");
    REQUIRE(texCount > 0);
    bool sawDds = false;
    for (whiteout::flakes::usize i = 0; i < texCount; ++i)
        sawDds = sawDds || std::string(tex[i]) == ".dds";
    CHECK(sawDds);

    auto [model, modelCount] = AltExtensionsFor(".mdx");
    REQUIRE(modelCount == 2);

    auto [none, noneCount] = AltExtensionsFor(".slk");
    CHECK(none == nullptr);
    CHECK(noneCount == 0);

    CHECK(ClassifyByExtension(".dds") == AssetKind::Texture);
    CHECK(ClassifyByExtension(".mdl") == AssetKind::Model);
    CHECK(ClassifyByExtension(".slk") == AssetKind::Other);
}

TEST_CASE("Each game's archive rules are its own", "[storage]") {
    const auto wc3 = DefaultArchives(ProductId::Wc3);
    REQUIRE(wc3.size() == 3);
    // Patch first: the chain returns the first hit, so an override has to
    // precede what it overrides.
    CHECK(wc3[0] == "War3Patch.mpq");
    CHECK(wc3[2] == "war3.mpq");

    // StarCraft II and Heroes are CASC-only in every version, so an archive
    // list here would name files that have never existed.
    CHECK(DefaultArchives(ProductId::Sc2).empty());

    const auto wow = DefaultArchives(ProductId::Wow);
    REQUIRE_FALSE(wow.empty());
    for (const auto& n : wow)
        CHECK(n.rfind("Data/", 0) == 0);

    // Only World of Warcraft scans: the others' names are fixed, so scanning
    // would be a directory walk that can only return what is already known.
    CHECK(ScanArchives(ProductId::Wc3, "D:/nowhere") == wc3);
    CHECK(ScanArchives(ProductId::Sc2, "D:/nowhere").empty());
    // A WoW install that is not there yields nothing rather than the static
    // list — reporting archives that cannot be opened would be a lie.
    CHECK(ScanArchives(ProductId::Wow, "D:/nowhere").empty());
}

TEST_CASE("A storage built from nothing misses cleanly", "[storage]") {
    // The unconfigured case has to be a well-formed empty storage, not a null
    // one: an unconfigured provider still answers reads, it just answers "no".
    auto storage = StorageBuilder(ProductId::Wc3).Build();
    REQUIRE(storage != nullptr);
    CHECK(storage->Game() == ProductId::Wc3);
    CHECK_FALSE(storage->HasCasc());
    CHECK_FALSE(storage->HasArchives());
    CHECK_FALSE(storage->HasListfile());
    CHECK(storage->CascRoots().empty());

    SourceRead hit;
    CHECK_FALSE(storage->Read("units/human/peasant.mdx", hit));
    CHECK_FALSE(storage->ReadById(21, hit));
    CHECK(hit.data.empty());
}

TEST_CASE("Roots and archives that are not there are skipped, not fatal", "[storage]") {
    // Every product is offered paths that may not exist — StarCraft II names
    // two installs of which one is usually absent, and WoW's archive set
    // varies by expansion. A miss at build time must leave a usable storage.
    auto storage = StorageBuilder(ProductId::Sc2)
                       .Casc("D:/nowhere/starcraft")
                       .Casc("D:/nowhere/heroes")
                       .Archives("D:/nowhere", {"absent.mpq"})
                       .Build();
    REQUIRE(storage != nullptr);
    CHECK_FALSE(storage->HasCasc());
    CHECK_FALSE(storage->HasArchives());

    SourceRead hit;
    CHECK_FALSE(storage->Read("assets/units/marine/marine.m3", hit));
}
