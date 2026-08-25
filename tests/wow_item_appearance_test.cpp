// ============================================================================
// Item skins — does the client database say what an item `.m2` won't?
//
// The other half of the problem wow_creature_skin_test.cpp covers. An item's
// object component declares texture type 2 and leaves TXID zero, because which
// skin belongs there is a property of the appearance the item was equipped as,
// not of the model — so a viewer that opens the file and nothing else draws a
// flat grey shield. Measured on a stock 11.x install, 562 of 593 sampled
// `item/objectcomponents/` models declare such a slot, which is why so many of
// them look untextured.
//
// Two halves are gated here: the join (ModelFileData → ItemDisplayInfo →
// ItemDisplayInfoModelMatRes → TextureFileData) against the corpus dump, and
// the whole feature end to end against a bare CASC root, with no listfile and
// no TACT keys — which is the configuration a stock install actually has.
//
// The corpus (`C:/Projects/WhiteoutLib/Corpus/WoW`, override with
// WDX_TEST_WOW_CORPUS) carries the `dbfilesclient/` dump but no item models, so
// the table half is checked by fileDataID against the community listfile for
// the same install. Skips when it is absent; skipped is not passed.
// ============================================================================

#include <catch2/catch_test_macros.hpp>

#include "io/file_content_provider.h"
#include "io/m2/m2_model_adapter.h"
#include "io/wow/item_appearance_table.h"
#include "renderer/profiles/wow/wow_replaceable_textures.h"
#include "whiteout/flakes/content_ref.h"

#include <whiteout/models/m2/m2.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <span>
#include <string>

using whiteout::flakes::ContentRef;
namespace io = whiteout::flakes::io;
namespace wow = whiteout::flakes::renderer::profiles::wow;
namespace fs = std::filesystem;

namespace {

fs::path CorpusRoot() {
    if (const char* v = std::getenv("WDX_TEST_WOW_CORPUS"); v && *v)
        return fs::path(v);
    return fs::path("C:/Projects/WhiteoutLib/Corpus/WoW");
}

// `item/objectcomponents/shield/shield_1h_artifactmagnar_d_03.m2` and the four
// `.blp` files the listfile places beside it: blue, green, red and yellow. The
// model names none of them — one mesh, four looks — so this is exactly the
// join, and the ids are fixture data read out of the listfile rather than
// anything the test computes.
constexpr whiteout::u32 kShield = 1241201;
constexpr whiteout::u32 kShieldSkins[] = {1241203, 1241204, 1241205, 1241206};

/// Which slot the object skin (`M2Texture::type` 2) lands in.
const whiteout::u32 kObjectSkinSlot =
    static_cast<whiteout::u32>(io::wow::ReplaceableSlotOfType(2));

std::size_t SlotsOfType(const whiteout::m2::Model& model, whiteout::u32 type) {
    return static_cast<std::size_t>(
        std::count_if(model.textures.begin(), model.textures.end(),
                      [&](const auto& t) { return t.type == type; }));
}

} // namespace

TEST_CASE("the item tables name the skin an .m2 leaves blank", "[m2][wow][db2]") {
    std::error_code ec;
    const fs::path root = CorpusRoot();
    if (!fs::is_directory(root / "dbfilesclient", ec)) {
        WARN("WoW corpus dbfilesclient/ not found — skipping");
        return;
    }

    io::FileContentProvider provider;
    provider.SetBasePath(root);

    io::wow::ItemAppearanceTable table;
    REQUIRE(table.Load(provider));
    // Tens of thousands, not a handful: a table that parsed but joined nothing
    // would satisfy a bare `Loaded()` and still leave every item grey.
    CHECK(table.ModelCount() > 10000);
    CHECK(table.AppearanceCount() > 10000);

    // fileDataID 0 is CASC's "unset", never a record, and a creature model is
    // not an item — a miss has to be a miss rather than an empty look.
    CHECK(table.ForModel(0).empty());
    CHECK(table.ForModel(123288).empty()); // creature/cow/cow.m2

    const auto looks = table.ForModel(kShield);
    REQUIRE_FALSE(looks.empty());
    for (const io::wow::ItemAppearance& look : looks) {
        INFO(look.displayId);
        // Every appearance of this model fills the object-skin slot and only
        // that one: the shield is one mesh whose whole difference is its
        // colour.
        CHECK(look.texture[kObjectSkinSlot] != 0);
        CHECK(std::find(std::begin(kShieldSkins), std::end(kShieldSkins),
                        look.texture[kObjectSkinSlot]) != std::end(kShieldSkins));
        for (whiteout::u32 slot = 0; slot < io::wow::kReplaceableSlots; ++slot)
            if (slot != kObjectSkinSlot)
                CHECK(look.texture[slot] == 0);
    }
    // All four colours are reachable, and by the lowest display id first, so
    // appearance 0 does not depend on how the table happened to be laid out.
    for (whiteout::u32 skin : kShieldSkins)
        CHECK(std::any_of(looks.begin(), looks.end(), [&](const io::wow::ItemAppearance& a) {
            return a.texture[kObjectSkinSlot] == skin;
        }));
    CHECK(looks.front().texture[kObjectSkinSlot] == kShieldSkins[0]);
    CHECK(std::is_sorted(looks.begin(), looks.end(),
                         [](const io::wow::ItemAppearance& a, const io::wow::ItemAppearance& b) {
                             return a.displayId < b.displayId;
                         }));
}

TEST_CASE("an item skins itself from a bare CASC root", "[m2][wow][db2]") {
    // The same claim one layer up, through the adapter that actually binds the
    // slot, off nothing but an install: no listfile, no TACT keys.
    io::FileContentProvider provider;
    provider.SetGame(whiteout::flakes::ProductId::Wow);
    if (provider.GamePath(whiteout::flakes::ProductId::Wow).empty() || !provider.HasCasc())
        SKIP("no World of Warcraft CASC install found");
    REQUIRE_FALSE(provider.HasListfile());

    const ContentRef ref = ContentRef::FromFileId(kShield);
    auto bytes = provider.ReadFile(ref);
    if (!bytes || bytes->empty())
        SKIP("the artifact shield is not in this build of the install");

    auto adapter = io::M2ModelAdapter::Load(
        ref, std::span<const whiteout::u8>(bytes->data(), bytes->size()), &provider);
    REQUIRE(adapter);
    REQUIRE(SlotsOfType(adapter->SourceModel(), 2) == 1); // the premise

    wow::WowReplaceableTextures replaceables;
    replaceables.SetContentProvider(&provider);
    CHECK(replaceables.Apply(*adapter, ref) == 1);

    // One picker entry per look, not per display record: the shield is named by
    // eight ItemDisplayInfo rows carrying four distinct textures between them,
    // and offering the same picture twice is offering nothing.
    const auto& offered = replaceables.Variations(ref);
    CHECK(offered.size() == std::size(kShieldSkins));

    // And the id it chose is readable — a slot filled with an id nothing can
    // produce renders exactly as grey as one that was never filled.
    std::size_t named = 0;
    for (const auto& td : adapter->GetTextures()) {
        if (td.sharedKey.empty())
            continue;
        ++named;
        INFO(td.sharedKey);
        REQUIRE(td.sharedKey[0] == '#');
        auto tex = provider.ReadFile(ContentRef::FromFileId(
            static_cast<whiteout::u32>(std::strtoul(td.sharedKey.c_str() + 1, nullptr, 10))));
        CHECK((tex && !tex->empty()));
    }
    CHECK(named == 2); // the object skin, plus the one the model names in TXID
}

TEST_CASE("a creature does not read the item tables", "[m2][wow][db2]") {
    // The two families are told apart by the types the model declares, and the
    // point of that is cost: the item tables are half a million rows, and a
    // session that only ever opens creatures should never touch them.
    std::error_code ec;
    const fs::path root = CorpusRoot();
    if (!fs::is_directory(root / "dbfilesclient", ec)) {
        WARN("WoW corpus dbfilesclient/ not found — skipping");
        return;
    }

    io::FileContentProvider provider;
    provider.SetBasePath(root);

    const std::string cow = "creature/cow/cow.m2";
    auto bytes = provider.ReadFile(cow);
    if (!bytes || bytes->empty())
        SKIP("creature/cow is not in the corpus");
    auto adapter = io::M2ModelAdapter::Load(
        ContentRef::FromPath(cow), std::span<const whiteout::u8>(bytes->data(), bytes->size()),
        &provider);
    REQUIRE(adapter);

    wow::WowReplaceableTextures replaceables;
    replaceables.SetContentProvider(&provider);
    replaceables.Apply(*adapter, ContentRef::FromPath(cow));
    CHECK_FALSE(replaceables.ItemTable().Loaded());
}
