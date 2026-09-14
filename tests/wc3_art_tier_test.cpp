// Warcraft III's TVFS mod chain: the three art tiers, the order each one
// reads its overlays in, and what a stored path says about which tier it
// belongs to.
//
// This is policy, not I/O — no install required. What it pins down is the
// part that went wrong when 3.0.0 shipped: `_de.w3mod:` was a mod root that
// nothing in the chain named, so every file only Definitive has resolved to
// nothing, and every path that arrived already carrying a chain was rescued
// only by the empty prefix on the end of the list.

#include <catch2/catch_test_macros.hpp>

#include "io/storage/storage_paths.h"

#include <algorithm>
#include <string>
#include <vector>

using whiteout::flakes::Wc3ArtTier;
using whiteout::flakes::io::HasWc3ModChain;
using whiteout::flakes::io::StripWc3ModRoot;
using whiteout::flakes::io::ToListingPath;
using whiteout::flakes::io::Wc3ModChain;
using whiteout::flakes::io::Wc3TierOfPath;

namespace {

std::vector<std::string> ChainOf(Wc3ArtTier tier) {
    std::vector<std::string> out;
    for (const char* p : Wc3ModChain(tier))
        out.emplace_back(p);
    return out;
}

constexpr const char* kSd = "war3.w3mod:";
constexpr const char* kHd = "war3.w3mod:_hd.w3mod:";
constexpr const char* kDe = "war3.w3mod:_de.w3mod:";

// Position of `prefix` in a tier's chain, or -1 when it is not in it.
int IndexIn(Wc3ArtTier tier, const char* prefix) {
    const std::vector<std::string> chain = ChainOf(tier);
    const auto it = std::find(chain.begin(), chain.end(), std::string(prefix));
    return it == chain.end() ? -1 : static_cast<int>(it - chain.begin());
}

} // namespace

TEST_CASE("each tier leads with its own overlay", "[wc3][tier]") {
    CHECK(ChainOf(Wc3ArtTier::Classic).front() == kSd);
    CHECK(ChainOf(Wc3ArtTier::Reforged).front() == kHd);
    CHECK(ChainOf(Wc3ArtTier::Definitive).front() == kDe);
}

TEST_CASE("Definitive falls through Reforged to Classic, in that order",
          "[wc3][tier]") {
    // Not cosmetic. 2,780 paths in a 3.0.0 install exist only under
    // `_hd.w3mod:` and 15,218 only under `war3.w3mod:`, so a Definitive read
    // that stopped at its own overlay would lose both sets; one that reached
    // Classic before Reforged would draw the classic art for everything the
    // remake replaced.
    const int de = IndexIn(Wc3ArtTier::Definitive, kDe);
    const int hd = IndexIn(Wc3ArtTier::Definitive, kHd);
    const int sd = IndexIn(Wc3ArtTier::Definitive, kSd);
    REQUIRE(de >= 0);
    REQUIRE(hd >= 0);
    REQUIRE(sd >= 0);
    CHECK(de < hd);
    CHECK(hd < sd);
}

TEST_CASE("the older tiers keep the order they had before 3.0.0",
          "[wc3][tier]") {
    // Classic and Reforged each lead with their own overlay and then reach
    // the other one; that fall-through is what lets the viewer show an asset
    // the selected tier never shipped. Definitive is not in either chain, so
    // nothing that resolved before 3.0.0 resolves differently now.
    CHECK(IndexIn(Wc3ArtTier::Classic, kSd) < IndexIn(Wc3ArtTier::Classic, kHd));
    CHECK(IndexIn(Wc3ArtTier::Reforged, kHd) < IndexIn(Wc3ArtTier::Reforged, kSd));
    CHECK(IndexIn(Wc3ArtTier::Classic, kDe) == -1);
    CHECK(IndexIn(Wc3ArtTier::Reforged, kDe) == -1);
}

TEST_CASE("a stored path names its own tier", "[wc3][tier]") {
    CHECK(Wc3TierOfPath("war3.w3mod:_de.w3mod:units\\forsaken\\x.mdx") ==
          Wc3ArtTier::Definitive);
    CHECK(Wc3TierOfPath("war3.w3mod:_hd.w3mod:units\\human\\footman\\footman.mdx") ==
          Wc3ArtTier::Reforged);
    CHECK(Wc3TierOfPath("war3.w3mod:units\\human\\footman\\footman.mdx") ==
          Wc3ArtTier::Classic);
    // Case is the storage's business, not the caller's.
    CHECK(Wc3TierOfPath("War3.w3mod:_DE.w3mod:Units\\X.mdx") == Wc3ArtTier::Definitive);
}

TEST_CASE("only the segment under the root decides the tier", "[wc3][tier]") {
    // A sub-mod of a tier still belongs to that tier, and a folder that merely
    // contains the text does not make one. Searching the whole string for
    // "_hd.w3mod" would get both of these wrong.
    CHECK(Wc3TierOfPath("war3.w3mod:_de.w3mod:_tilesets\\a.w3mod:doodads\\x.mdx") ==
          Wc3ArtTier::Definitive);
    CHECK(Wc3TierOfPath("war3.w3mod:maps\\_hd.w3mod_backup\\x.mdx") == Wc3ArtTier::Classic);
}

TEST_CASE("a path with no mod chain names no tier", "[wc3][tier]") {
    // An MPQ entry, a loose file, another game's storage. Answering "Classic"
    // for these would pin a StarCraft II scene to a Warcraft III overlay.
    CHECK_FALSE(Wc3TierOfPath("units\\human\\footman\\footman.mdx").has_value());
    CHECK_FALSE(Wc3TierOfPath("C:\\Games\\model.mdx").has_value());
    CHECK_FALSE(Wc3TierOfPath("").has_value());
    CHECK_FALSE(HasWc3ModChain("mods\\liberty.sc2mod\\assets\\x.dds"));
}

TEST_CASE("the mod root strips off, the overlay does not", "[wc3][tier]") {
    // What the browser lists. Each overlay has to survive as a folder or three
    // different files collapse into one entry — which is exactly what dropping
    // everything up to the last ':' does.
    CHECK(StripWc3ModRoot("war3.w3mod:_de.w3mod:units\\x.mdx") == "_de.w3mod:units\\x.mdx");
    CHECK(StripWc3ModRoot("war3.w3mod:units\\x.mdx") == "units\\x.mdx");
    CHECK(StripWc3ModRoot("units\\x.mdx") == "units\\x.mdx");
    // ToListingPath is the other one, and it DOES flatten the chain — it is
    // for a path being handed back to a read, not for one being shown.
    CHECK(ToListingPath("war3.w3mod:_de.w3mod:Units\\X.mdx") == "units/x.mdx");
}
