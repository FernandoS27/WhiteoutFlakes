// ============================================================================
// D3 item equipping — gate D3-G7, offline.
//
// The dressing room's claim is "equip by in-game name": a GameBalance item
// name resolves through its gbid to an Actor, the Actor's two look tags pick
// a wardrobe piece, and the result must be EXACTLY what manual dressing of
// the same piece produces. The checks below split the claim the way the
// pipeline splits:
//
//   * the registry half — names, gbids, actors and per-slot classification
//     read out of the corpus GameBalance snapshot;
//   * the resolver half — known items resolve to the (lookValue,
//     lookNameHash, snoActor) their Actors really carry, pinned against the
//     values measured once by hand;
//   * the parity half — dressing a player via an item produces a draw set
//     byte-identical to manual dressing, and the engine's fallback chain
//     lands a value the class lacks exactly where 0x7544A0 would.
//
// Corpus root: WDX_TEST_D3_CORPUS, default C:/Projects/WhiteoutLib/Corpus/D3.
// The GameBalance half needs the Corpus/D3/GameBalance snapshot (build
// 2.8.0.99920); skipped is not passed.
// ============================================================================

#include <catch2/catch_test_macros.hpp>

#include "io/d3/d3_effect_resolver.h"
#include "io/d3/d3_item_registry.h"
#include "io/d3/d3_model_adapter.h"
#include "io/d3/d3_sno_cache.h"
#include "renderer/profiles/diablo3/d3_character_appearance.h"
#include "whiteout/flakes/content_ref.h"

#include <whiteout/sno/d3/native/character.h>
#include <whiteout/sno/d3/native/d3_native.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
namespace d3n = ::whiteout::sno::d3::native;
namespace d3p = ::whiteout::flakes::renderer::profiles::diablo3;
using namespace ::whiteout;
using ::whiteout::flakes::ContentRef;

namespace {

fs::path CorpusRoot() {
    if (const char* v = std::getenv("WDX_TEST_D3_CORPUS"); v && *v)
        return fs::path(v);
    return fs::path("C:/Projects/WhiteoutLib/Corpus/D3");
}

std::vector<u8> ReadAll(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f)
        return {};
    f.seekg(0, std::ios::end);
    const auto n = static_cast<std::size_t>(f.tellg());
    f.seekg(0);
    std::vector<u8> b(n);
    f.read(reinterpret_cast<char*>(b.data()), static_cast<std::streamsize>(n));
    return b;
}

/// The three fixture items, measured once by hand against the snapshot and
/// the corpus actors (CoreTOC joins the ids to the names). Pinned values,
/// not derived ones: a drift here is a parser or hash regression.
struct Fixture {
    const char* item;
    i32 snoActor;
    const char* actorFile;
};
constexpr Fixture kChest = {"Unique_Chest_001", 205616, "chestArmor_norm_unique_089.acr"};
constexpr Fixture kHelm = {"Unique_Helm_001", 220630, "Helm_norm_unique_02.acr"};
constexpr Fixture kDagger = {"Unique_Dagger_003", 195174, "Dagger_norm_unique_03.acr"};
constexpr Fixture kSword = {"Unique_Sword_1H_017", 115140, "Sword_norm_unique_01.acr"};
constexpr Fixture kShield = {"Unique_Shield_011", 152667, "Shield_norm_unique_04.acr"};
constexpr Fixture kShoulder = {"Unique_Shoulder_001", 198573, "shoulderPads_norm_unique_01.acr"};

/// Registry over the corpus snapshot, with the fixture actors seeded into the
/// cache first so classification can read their tags without a storage. The
/// registry reads its Actors from a directory now, not a cache, so the same
/// six are copied into a scratch directory — pointing it at the full corpus
/// Actor tree would classify ~3.4k items and change what this rig measures.
struct Rig {
    flakes::io::D3SnoCache cache{nullptr};
    flakes::io::D3ItemRegistry items;
    bool ok = false;

    Rig() {
        const fs::path root = CorpusRoot();
        std::error_code ec;
        if (!fs::is_directory(root / "GameBalance", ec))
            return;
        const fs::path actorDir = fs::temp_directory_path(ec) / "wdx_d3_item_equip_actors";
        fs::create_directories(actorDir, ec);
        for (const auto& fx : {kChest, kHelm, kDagger, kSword, kShield, kShoulder}) {
            const auto bytes = ReadAll(root / "Actor" / fx.actorFile);
            if (bytes.empty())
                continue;
            cache.AdoptActor(fx.snoActor, bytes);
            fs::copy_file(root / "Actor" / fx.actorFile, actorDir / fx.actorFile,
                          fs::copy_options::overwrite_existing, ec);
        }
        items.SetFallbackDirectory(root / "GameBalance");
        items.SetFallbackActorDirectory(actorDir);
        items.SetFallbackStringListDirectory(root / "StringList");
        ok = items.EnsureBuilt(nullptr);
    }
};

} // namespace

TEST_CASE("D3-G7: the registry reads the snapshot", "[d3][items][corpus]") {
    Rig rig;
    if (!rig.ok) {
        WARN("No Corpus/D3/GameBalance snapshot. SKIPPED, not passed.");
        return;
    }
    // The whole client-side item population, exactly (README of the snapshot).
    CHECK(rig.items.Items().size() == 3686);

    const auto* dagger = rig.items.FindByName(kDagger.item);
    REQUIRE(dagger != nullptr);
    CHECK(dagger->gbid == 0x4239EDB2u); // gbidHash("Unique_Dagger_003"), by hand
    CHECK(dagger->snoActor == kDagger.snoActor);
    CHECK(dagger->gbidItemType == d3n::gbidHash("Dagger"));
    // The find is the engine's find: through the case-insensitive hash.
    CHECK(rig.items.FindByName("UNIQUE_dagger_003") == dagger);
    CHECK(rig.items.FindByGbid(dagger->gbid) == dagger);
    CHECK(rig.items.FindByName("No_Such_Item_Anywhere") == nullptr);

    // Type names crack back out of the hashes.
    CHECK(flakes::io::D3ItemRegistry::ItemTypeName(dagger->gbidItemType) == "Dagger");
    CHECK(flakes::io::D3ItemRegistry::ItemTypeName(d3n::gbidHash("CrusaderShield")) ==
          "CrusaderShield");
}

TEST_CASE("D3-G7: classification reads the Actors", "[d3][items][corpus]") {
    Rig rig;
    if (!rig.ok) {
        WARN("No Corpus/D3/GameBalance snapshot. SKIPPED, not passed.");
        return;
    }
    using VS = d3n::EVisualSlot;

    // The chest: an armour item. Tags measured off its corpus Actor.
    const auto* chest = rig.items.FindByName(kChest.item);
    REQUIRE(chest != nullptr);
    REQUIRE(chest->lookValue.has_value());
    CHECK(*chest->lookValue == 5u); // HVY_A
    REQUIRE(chest->lookNameHash.has_value());
    CHECK(*chest->lookNameHash == 0x828E211Fu);
    CHECK(chest->CanGo(VS::Torso));       // ChestArmor narrows the offer
    CHECK_FALSE(chest->CanGo(VS::Legs));
    CHECK_FALSE(chest->CanGo(VS::RightHand));

    // The helm: an attachment with per-class art and the hair tag path.
    const auto* helm = rig.items.FindByName(kHelm.item);
    REQUIRE(helm != nullptr);
    CHECK(helm->hasPerClassArt);
    CHECK(helm->CanGo(VS::Head));
    CHECK_FALSE(helm->CanGo(VS::Torso));

    // The dagger: held, one-handed, either hand; never an armour slot.
    const auto* dagger = rig.items.FindByName(kDagger.item);
    REQUIRE(dagger != nullptr);
    CHECK(dagger->hasHoldType);
    CHECK_FALSE(dagger->lookValue.has_value());
    CHECK(dagger->CanGo(VS::RightHand));
    CHECK(dagger->CanGo(VS::LeftHand));
    CHECK_FALSE(dagger->CanGo(VS::Torso));
}

TEST_CASE("D3-G7: resolveEquip matches the measured tags", "[d3][items][corpus]") {
    Rig rig;
    if (!rig.ok) {
        WARN("No Corpus/D3/GameBalance snapshot. SKIPPED, not passed.");
        return;
    }
    using VS = d3n::EVisualSlot;
    using PC = d3n::PlayerClass;
    using G = d3n::Gender;

    const auto helmActor = rig.cache.Actor(kHelm.snoActor);
    REQUIRE(helmActor != nullptr);
    const auto* helm = rig.items.FindByName(kHelm.item);
    REQUIRE(helm != nullptr);
    const auto traits = d3n::itemTypeTraits(helm->gbidItemType);

    // Per-class art, measured off the Actor: 0x17000 -> 220620 (male barb),
    // 0x17001 -> 220540 (female barb). The +1-is-female direction was proved
    // on Helm_hell_base_01 (barbM/barbF actor names in CoreTOC).
    const auto male = d3n::resolveEquip(*helmActor, traits, VS::Head, PC::Barbarian, G::Male,
                                        /*sheathed*/ false);
    CHECK(male.drawn);
    CHECK(male.hardpoint == "HP_helm");
    CHECK(male.attachActorSno == 220620);
    const auto female = d3n::resolveEquip(*helmActor, traits, VS::Head, PC::Barbarian, G::Female,
                                          /*sheathed*/ false);
    CHECK(female.attachActorSno == 220540);

    // The dagger holds with type 2: by slot.
    const auto daggerActor = rig.cache.Actor(kDagger.snoActor);
    REQUIRE(daggerActor != nullptr);
    const auto rh = d3n::resolveEquip(*daggerActor, {}, VS::RightHand, PC::Barbarian, G::Male,
                                      /*sheathed*/ false);
    CHECK(rh.drawn);
    CHECK(rh.hardpoint == "HP_rightWeapon");
    CHECK(rh.attachActorSno == kDagger.snoActor);
}

TEST_CASE("D3-G7: item dressing equals manual dressing", "[d3][items][corpus]") {
    Rig rig;
    if (!rig.ok) {
        WARN("No Corpus/D3/GameBalance snapshot. SKIPPED, not passed.");
        return;
    }
    const auto bytes = ReadAll(CorpusRoot() / "Appearances" / "Barbarian_Male.app");
    if (bytes.empty()) {
        WARN("No Barbarian_Male.app in the corpus. SKIPPED, not passed.");
        return;
    }
    auto adapter = flakes::io::D3ModelAdapter::LoadAppearance(
        ContentRef::FromPath("Barbarian_Male.app"), bytes, rig.cache);
    REQUIRE(adapter != nullptr);

    const auto* chest = rig.items.FindByName(kChest.item);
    REQUIRE(chest != nullptr);
    const auto chestActor = rig.cache.Actor(chest->snoActor);
    REQUIRE(chestActor != nullptr);

    // ---- The item path.
    d3p::D3CharacterAppearance viaItem;
    REQUIRE(viaItem.IsCharacter(*adapter));
    REQUIRE(viaItem.SetOutfitItem(*adapter, d3n::EVisualSlot::Torso, chest, chestActor));
    REQUIRE(viaItem.Apply(*adapter));
    const auto hiddenSpan = adapter->GeosetHidden();
    const std::vector<u8> itemHidden(hiddenSpan.begin(), hiddenSpan.end());
    const auto looksSpan = adapter->GeosetLooks();
    const std::vector<u32> itemLooks(looksSpan.begin(), looksSpan.end());

    // What the item landed on: the torso piece whose look value is the tag's.
    u32 torsoValue = 0;
    for (const auto& s : viaItem.Slots(*adapter)) {
        if (s.slot != d3n::LookSlot::Torso)
            continue;
        REQUIRE(s.selectedItem < s.items.size());
        torsoValue = s.items[s.selectedItem].lookValue;
    }
    CHECK(torsoValue == *chest->lookValue);

    // ---- The manual path: same piece, same look, chosen by hand.
    d3p::D3CharacterAppearance manual;
    REQUIRE(manual.IsCharacter(*adapter));
    for (const auto& s : manual.Slots(*adapter)) {
        if (s.slot != d3n::LookSlot::Torso)
            continue;
        for (u32 i = 0; i < static_cast<u32>(s.items.size()); ++i) {
            if (s.items[i].lookValue == *chest->lookValue)
                manual.SetItem(*adapter, d3n::LookSlot::Torso, i);
        }
    }
    u32 lookIndex = 0;
    const auto& looks = adapter->Looks();
    for (std::size_t i = 0; i < looks.size(); ++i) {
        if (d3n::lookNameHash33(looks[i]) == *chest->lookNameHash)
            lookIndex = static_cast<u32>(i);
    }
    manual.SetSlotLook(*adapter, d3n::LookSlot::Torso, lookIndex);
    REQUIRE(manual.Apply(*adapter));

    const auto manualHiddenSpan = adapter->GeosetHidden();
    const auto manualLooksSpan = adapter->GeosetLooks();
    CHECK(std::vector<u8>(manualHiddenSpan.begin(), manualHiddenSpan.end()) == itemHidden);
    CHECK(std::vector<u32>(manualLooksSpan.begin(), manualLooksSpan.end()) == itemLooks);

    // ---- The fallback chain, where the class lacks the value.
    // LIT_C (7) is not in the Barbarian wardrobe; the engine retries with 1
    // (LIT_A). An out-of-table value degrades all the way to naked.
    d3n::Actor synthetic;
    synthetic.dwSnoId = 1;
    synthetic.arTagMap = {{0, d3n::kTagItemLookValue, 7},
                          {0, d3n::kTagItemLookName, d3n::lookNameHash33("A")}};
    flakes::io::D3ItemRecord fake;
    fake.name = "Synthetic_LitC";
    fake.gbid = d3n::gbidHash(fake.name);
    d3p::D3CharacterAppearance fb;
    auto syntheticPtr = std::make_shared<d3n::Actor>(synthetic);
    REQUIRE(fb.SetOutfitItem(*adapter, d3n::EVisualSlot::Torso, &fake, syntheticPtr));
    for (const auto& s : fb.Slots(*adapter)) {
        if (s.slot != d3n::LookSlot::Torso)
            continue;
        CHECK(s.items[s.selectedItem].lookValue == d3n::fallbackLookValue(7));
    }
    auto synthetic42 = std::make_shared<d3n::Actor>(synthetic);
    synthetic42->arTagMap[0].dwValue = 42;
    REQUIRE(fb.SetOutfitItem(*adapter, d3n::EVisualSlot::Torso, &fake, synthetic42));
    for (const auto& s : fb.Slots(*adapter)) {
        if (s.slot != d3n::LookSlot::Torso)
            continue;
        CHECK(s.items[s.selectedItem].lookValue == 0u);
    }

    // ---- Dye 1 dresses the slot naked; clearing it re-applies the item.
    d3p::D3CharacterAppearance hide;
    REQUIRE(hide.SetOutfitItem(*adapter, d3n::EVisualSlot::Torso, chest, chestActor));
    hide.SetOutfitDye(*adapter, d3n::EVisualSlot::Torso, d3n::kDyeHidden);
    for (const auto& s : hide.Slots(*adapter)) {
        if (s.slot == d3n::LookSlot::Torso)
            CHECK(s.items[s.selectedItem].lookValue == 0u);
    }
    hide.SetOutfitDye(*adapter, d3n::EVisualSlot::Torso, d3n::kDyeNone);
    for (const auto& s : hide.Slots(*adapter)) {
        if (s.slot == d3n::LookSlot::Torso)
            CHECK(s.items[s.selectedItem].lookValue == *chest->lookValue);
    }
}

TEST_CASE("D3-G7: attachments land on real hardpoints, sheathe moves them",
          "[d3][items][corpus]") {
    Rig rig;
    if (!rig.ok) {
        WARN("No Corpus/D3/GameBalance snapshot. SKIPPED, not passed.");
        return;
    }
    const auto bytes = ReadAll(CorpusRoot() / "Appearances" / "Barbarian_Male.app");
    if (bytes.empty()) {
        WARN("No Barbarian_Male.app in the corpus. SKIPPED, not passed.");
        return;
    }
    auto adapter = flakes::io::D3ModelAdapter::LoadAppearance(
        ContentRef::FromPath("Barbarian_Male.app"), bytes, rig.cache);
    REQUIRE(adapter != nullptr);
    using VS = d3n::EVisualSlot;

    d3p::D3CharacterAppearance chars;
    REQUIRE(chars.IsCharacter(*adapter));
    chars.SetOutfitBody(*adapter, d3n::PlayerClass::Barbarian, d3n::Gender::Male);
    for (const auto& [fx, slot] :
         {std::pair{&kHelm, VS::Head}, std::pair{&kSword, VS::RightHand},
          std::pair{&kShield, VS::LeftHand}, std::pair{&kShoulder, VS::Shoulders}}) {
        const auto* rec = rig.items.FindByName(fx->item);
        REQUIRE(rec != nullptr);
        auto actor = rig.cache.Actor(fx->snoActor);
        REQUIRE(actor != nullptr);
        REQUIRE(chars.SetOutfitItem(*adapter, slot, rec, actor));
    }

    // Drawn: five children (the shoulders twice), each on a hardpoint the
    // appearance really ships — names AND bone indices.
    auto drawn = chars.OutfitAttachments(*adapter);
    REQUIRE(drawn.size() == 5);
    auto hpOf = [&](VS slot) {
        std::vector<std::string_view> hps;
        for (const auto& a : drawn)
            if (a.visualSlot == static_cast<i32>(slot))
                hps.push_back(a.hardpoint);
        return hps;
    };
    CHECK(hpOf(VS::Head) == std::vector<std::string_view>{"HP_helm"});
    CHECK(hpOf(VS::RightHand) == std::vector<std::string_view>{"HP_rightWeapon"});
    // A shipped shield Actor carries NO hold-type tag, and the engine's
    // absent-tag default is 0 (the tag descriptor at 0x14850DC defaults
    // through the shared zero cell), so a drawn shield rides HP_leftWeapon --
    // the by-slot case -- and the flag-26 -> HP_shield fall-through is close
    // to dead code for drawn shields. Verified visually: the shield sits on
    // the left forearm.
    CHECK(hpOf(VS::LeftHand) == std::vector<std::string_view>{"HP_leftWeapon"});
    CHECK(hpOf(VS::Shoulders) ==
          std::vector<std::string_view>{"HP_left_shoulderPad", "HP_right_shoulderPad"});
    for (const auto& a : drawn) {
        INFO(std::string(a.hardpoint));
        CHECK(a.actorSno > 0);
        CHECK(flakes::io::d3::FindD3Hardpoint(adapter->SourceAppearance(), a.hardpoint) >= 0);
        CHECK_FALSE(a.sheathed);
    }

    // Sheathed: same five children — the weapons re-resolve to the sheath
    // hardpoints, the helm and shoulders stay where they are.
    chars.SetOutfitSheathed(*adapter, true);
    auto sheathed = chars.OutfitAttachments(*adapter);
    REQUIRE(sheathed.size() == 5);
    auto hpOf2 = [&](VS slot) {
        std::vector<std::string_view> hps;
        for (const auto& a : sheathed)
            if (a.visualSlot == static_cast<i32>(slot))
                hps.push_back(a.hardpoint);
        return hps;
    };
    CHECK(hpOf2(VS::RightHand) == std::vector<std::string_view>{"HP_sheath_right_Back"});
    CHECK(hpOf2(VS::LeftHand) == std::vector<std::string_view>{"HP_sheath_Shield"});
    CHECK(hpOf2(VS::Head) == std::vector<std::string_view>{"HP_helm"});
    CHECK(hpOf2(VS::Shoulders) ==
          std::vector<std::string_view>{"HP_left_shoulderPad", "HP_right_shoulderPad"});
    for (const auto& a : sheathed) {
        INFO(std::string(a.hardpoint));
        CHECK(flakes::io::d3::FindD3Hardpoint(adapter->SourceAppearance(), a.hardpoint) >= 0);
    }

    // The model side of per-class art: the helm's child is the barbM actor,
    // not the item's own (measured pair on Helm_norm_unique_02).
    bool sawHelm = false;
    for (const auto& a : drawn) {
        if (a.visualSlot == static_cast<i32>(VS::Head)) {
            CHECK(a.actorSno == 220620);
            sawHelm = true;
        }
    }
    CHECK(sawHelm);
}

TEST_CASE("D3-G7: display names, sets and classes", "[d3][items][corpus]") {
    Rig rig;
    if (!rig.ok) {
        WARN("No Corpus/D3/GameBalance snapshot. SKIPPED, not passed.");
        return;
    }
    if (rig.items.Sets().empty()) {
        WARN("No Corpus/D3/StringList snapshot. SKIPPED, not passed.");
        return;
    }
    using PC = d3n::PlayerClass;

    // ---- Display names: Items.stl joined on the gbid. Exact counts, measured
    // once against the snapshot pair (3,119 of the 3,122 named have art).
    size_t named = 0;
    for (const auto& rec : rig.items.Items())
        named += !rec.displayName.empty();
    CHECK(named == 3122);
    const auto* goldskin = rig.items.FindByName("Unique_Chest_001");
    REQUIRE(goldskin != nullptr);
    CHECK(goldskin->displayName == "Goldskin");

    // ---- Sets: every record's gbidSet resolves against ItemSets.stl (the
    // items use 127 of its 129 keys), and the join carries the display name.
    CHECK(rig.items.Sets().size() == 127);
    for (const auto& rec : rig.items.Items()) {
        if (rec.gbidSet == 0xFFFFFFFFu)
            continue;
        const auto* set = rig.items.SetOf(rec);
        REQUIRE(set != nullptr);
        CHECK_FALSE(set->key.empty());
        CHECK_FALSE(set->displayName.empty());
    }

    // ---- Types: the STL keys close what the builtin table cannot know.
    CHECK(rig.items.TypeNameOf(d3n::gbidHash("ChestArmor_Wizard")) == "ChestArmor_Wizard");
    CHECK(flakes::io::D3ItemRegistry::ItemTypeName(d3n::gbidHash("ChestArmor_Wizard")).empty());

    // ---- Classes, all three derivation tiers plus the neutral floor. The
    // total is exact: 937 class-typed + 79 set-inherited + 36 from the six
    // curated all-generic-typed sets.
    size_t singles = 0;
    for (const auto& rec : rig.items.Items()) {
        const u32 m = rec.classMask;
        singles += m != 0 && (m & (m - 1)) == 0;
    }
    CHECK(singles == 1052);
    // Type tier: Firebird's chest is a ChestArmor_Wizard.
    const auto* firebird = rig.items.FindByName("Unique_Chest_Set_06_x1");
    REQUIRE(firebird != nullptr);
    CHECK(firebird->CanWear(PC::Wizard));
    CHECK_FALSE(firebird->CanWear(PC::Barbarian));
    // Curated tier: The Shadow's Mantle is all generic-typed, classed by key.
    const auto* mantle = rig.items.FindByName("Unique_Chest_Set_14_x1");
    REQUIRE(mantle != nullptr);
    const auto* mantleSet = rig.items.SetOf(*mantle);
    REQUIRE(mantleSet != nullptr);
    CHECK(mantleSet->key == "Ninja_Set_x1");
    CHECK(mantle->CanWear(PC::DemonHunter));
    CHECK_FALSE(mantle->CanWear(PC::Monk));
    // Neutral floor: Goldskin and the fixture dagger fit anyone.
    for (whiteout::i32 c = 0; c < static_cast<whiteout::i32>(d3n::kPlayerClassCount); ++c) {
        CHECK(goldskin->CanWear(static_cast<PC>(c)));
        CHECK(rig.items.FindByName(kDagger.item)->CanWear(static_cast<PC>(c)));
    }
}
