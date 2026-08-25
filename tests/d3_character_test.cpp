// ============================================================================
// D3 character dressing — gate D3-G6, offline.
//
// A player Appearance ships its whole wardrobe: naked, light, medium and heavy
// for four slots, the helmet cutaways of the hair, and the decapitated,
// dismembered and skeletal bodies. Nothing in the file says which of them draw.
// Before D3CharacterAppearance every one of them did, which is what made the
// Barbarian's baseline trace 30 sub-objects of interpenetrating armour.
//
// THE DISCRIMINATOR
// -----------------
// "The character looks right" is not a signal here, because a character with
// nothing hidden also looks like a character — a fully-dressed one, at a glance,
// with the naked body inside it. So the checks below are counting checks:
//
//   * a dressed character hides MORE than it shows, and hides the *other three*
//     weights of every slot it shows one of;
//   * exactly one piece per slot survives, never zero (a rule that hid
//     everything would pass a "no overlap" test perfectly); and
//   * a model with no wardrobe is left completely alone, which is the half a
//     rule applied too eagerly breaks and no player-model test can see.
//
// Corpus root: WDX_TEST_D3_CORPUS, default C:/Projects/WhiteoutLib/Corpus/D3.
// Skipped is not passed.
// ============================================================================

#include <catch2/catch_test_macros.hpp>

#include "io/d3/d3_model_adapter.h"
#include "io/d3/d3_sno_cache.h"
#include "io/file_content_provider.h"
#include "io/storage_browser.h"
#include "renderer/profiles/diablo3/d3_character_appearance.h"
#include "whiteout/flakes/content_ref.h"

#include <whiteout/sno/d3/native/character.h>
#include <whiteout/sno/d3/native/d3_native.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
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

/// The fourteen shipped player Appearances, by their own stems. Loaded through
/// the adapter rather than the bare parser so the wardrobe is read off the
/// *emission order* — the index space the hidden set and the look overrides
/// both live in, and the one a bare parse does not have.
std::vector<std::pair<std::string, std::shared_ptr<flakes::io::D3ModelAdapter>>> LoadPlayers(
    flakes::io::D3SnoCache& cache) {
    std::vector<std::pair<std::string, std::shared_ptr<flakes::io::D3ModelAdapter>>> out;
    const fs::path dir = CorpusRoot() / "Appearances";
    std::error_code ec;
    if (!fs::is_directory(dir, ec))
        return out;
    for (i32 c = 0; c < static_cast<i32>(d3n::kPlayerClassCount); ++c) {
        for (auto g : {d3n::Gender::Male, d3n::Gender::Female}) {
            const std::string stem =
                d3n::playerAppearanceStem(static_cast<d3n::PlayerClass>(c), g);
            const fs::path p = dir / (stem + ".app");
            const auto bytes = ReadAll(p);
            if (bytes.empty())
                continue;
            auto a = flakes::io::D3ModelAdapter::LoadAppearance(
                ContentRef::FromPath(p.string()), bytes, cache);
            if (a)
                out.emplace_back(stem, std::move(a));
        }
    }
    return out;
}

} // namespace

TEST_CASE("D3 corpus: every player Appearance offers a wardrobe", "[d3][character][corpus]") {
    flakes::io::D3SnoCache cache(nullptr);
    const auto players = LoadPlayers(cache);
    if (players.empty()) {
        WARN("No D3 corpus at " << CorpusRoot().string()
                                << " (set WDX_TEST_D3_CORPUS). SKIPPED, not passed.");
        return;
    }
    // 7 classes x 2 genders. Pinned rather than "some", because two of the
    // stems (X1_Crusader, P6_Necro) do not follow from the class name and a
    // regression there is a silently smaller sweep.
    REQUIRE(players.size() == 14);

    d3p::D3CharacterAppearance chars;
    for (const auto& [stem, adapter] : players) {
        INFO(stem);
        REQUIRE(chars.IsCharacter(*adapter));

        const auto slots = chars.Slots(*adapter);
        // All four armour slots, every time. Hair is the one that varies —
        // WitchDoctor_Male ships none at all — so it is not required here.
        std::set<d3n::LookSlot> seen;
        for (const auto& s : slots)
            seen.insert(s.slot);
        for (auto want : {d3n::LookSlot::Torso, d3n::LookSlot::Legs, d3n::LookSlot::Boots,
                          d3n::LookSlot::Gloves}) {
            INFO("slot " << static_cast<int>(want));
            CHECK(seen.count(want) == 1);
        }

        for (const auto& s : slots) {
            INFO(stem << " / " << s.name);
            CHECK(s.items.size() >= 1);
            // Every piece owns at least one geoset, and no two pieces of one
            // slot share one: a sub-object belongs to exactly one thing a
            // player can wear, or "one piece per slot" is not a partition and
            // showing one would show two.
            std::set<u32> claimed;
            for (const auto& item : s.items) {
                INFO("  item " << item.label);
                CHECK(!item.geosets.empty());
                for (const u32 g : item.geosets)
                    CHECK(claimed.insert(g).second);
            }
            if (s.slot == d3n::LookSlot::Hair)
                continue;
            // The four weights are what the look value selects between, and an
            // armour slot with fewer than all four cannot answer a value the
            // fallback chain lands on.
            std::set<d3n::ArmourWeight> weights;
            for (const auto& item : s.items)
                weights.insert(item.weight);
            for (auto w : {d3n::ArmourWeight::Naked, d3n::ArmourWeight::Light,
                           d3n::ArmourWeight::Medium, d3n::ArmourWeight::Heavy}) {
                INFO("  weight " << static_cast<int>(w));
                CHECK(weights.count(w) == 1);
            }
            // Item 0 is the default and the default is naked, because
            // Appearance_GetDefaultLook hands out value 0. A host that shows
            // item 0 with nothing chosen shows an unequipped character.
            CHECK(s.items[0].weight == d3n::ArmourWeight::Naked);
            CHECK(s.items[0].lookValue == 0u);
        }
    }
}

TEST_CASE("D3 corpus: dressing hides the rest of the wardrobe", "[d3][character][corpus]") {
    flakes::io::D3SnoCache cache(nullptr);
    const auto players = LoadPlayers(cache);
    if (players.empty()) {
        WARN("No D3 corpus. SKIPPED, not passed.");
        return;
    }
    d3p::D3CharacterAppearance chars;

    std::size_t totalGeosets = 0, totalShown = 0;
    for (const auto& [stem, adapter] : players) {
        INFO(stem);
        REQUIRE(chars.Apply(*adapter));

        const auto hidden = adapter->GeosetHidden();
        const std::size_t count = adapter->EmittedSubObjects().size();
        REQUIRE(hidden.size() == count);
        REQUIRE(adapter->GeosetLooks().size() == count);

        std::size_t shown = 0;
        for (const u8 h : hidden)
            shown += (h == 0) ? 1 : 0;
        totalGeosets += count;
        totalShown += shown;

        // Exactly the chosen piece of each slot, and nothing else: the extras
        // start hidden because nothing in ActorModel_ApplyLook switches them —
        // a decapitated body is gameplay, not equipment.
        const auto slots = chars.Slots(*adapter);
        std::size_t expected = 0;
        for (const auto& s : slots) {
            REQUIRE(s.selectedItem < s.items.size());
            const auto& worn = s.items[s.selectedItem];
            expected += worn.geosets.size();
            for (const u32 g : worn.geosets) {
                INFO(stem << " wears " << s.name << " " << worn.label << " geoset " << g);
                CHECK(hidden[g] == 0);
            }
            // ...and every piece it is worn *instead of* is held back.
            for (std::size_t i = 0; i < s.items.size(); ++i) {
                if (i == s.selectedItem)
                    continue;
                for (const u32 g : s.items[i].geosets) {
                    INFO(stem << " / " << s.name << " not-worn geoset " << g);
                    CHECK(hidden[g] == 1);
                }
            }
        }
        for (const auto& ex : chars.Extras(*adapter)) {
            INFO(stem << " extra " << ex.name);
            CHECK(!ex.shown);
            CHECK(hidden[ex.geoset] == 1);
        }
        CHECK(shown == expected);
        // Never nothing. A rule that hid the whole model would satisfy every
        // "no two pieces overlap" check above perfectly.
        CHECK(shown > 0);

        // Undressed, the looks are uniform, so the canonical texture list is
        // byte-identical to the one the two-argument form produces. That is
        // what keeps a spawn that never touches the wardrobe off a different
        // texture-id space from the one every existing gate recorded.
        const auto uniform = flakes::io::CollectD3Textures(adapter->SourceAppearance(),
                                                           adapter->LookIndex());
        const auto widened = flakes::io::CollectD3Textures(
            adapter->SourceAppearance(), adapter->LookIndex(), adapter->EmittedSubObjects(),
            adapter->GeosetLooks());
        REQUIRE(uniform.size() == widened.size());
        for (std::size_t i = 0; i < uniform.size(); ++i)
            CHECK(uniform[i].snoId == widened[i].snoId);
    }

    std::printf("[d3-char] %zu player geoset(s), %zu shown when dressed (%.0f%%)\n", totalGeosets,
                totalShown,
                totalGeosets ? 100.0 * static_cast<double>(totalShown) /
                                   static_cast<double>(totalGeosets)
                             : 0.0);
    // A dressed character shows a minority of what it carries. Stated as a
    // bound rather than a number so it reads as the claim it is; measured at
    // roughly a quarter.
    CHECK(totalShown * 2 < totalGeosets);
}

TEST_CASE("D3 corpus: a slot look reaches only that slot's geosets", "[d3][character][corpus]") {
    flakes::io::D3SnoCache cache(nullptr);
    const auto players = LoadPlayers(cache);
    if (players.empty()) {
        WARN("No D3 corpus. SKIPPED, not passed.");
        return;
    }
    d3p::D3CharacterAppearance chars;

    std::size_t widened = 0;
    for (const auto& [stem, adapter] : players) {
        INFO(stem);
        const auto looks = adapter->Looks();
        REQUIRE(looks.size() > 1);

        // Dress the torso in the heaviest thing it has, out of the LAST look
        // the file offers — a set nothing else is wearing.
        const auto before = chars.Slots(*adapter);
        auto torso = std::find_if(before.begin(), before.end(), [](const d3p::D3WardrobeSlot& s) {
            return s.slot == d3n::LookSlot::Torso;
        });
        REQUIRE(torso != before.end());
        const u32 piece = static_cast<u32>(torso->items.size() - 1);
        const u32 look = static_cast<u32>(looks.size() - 1);
        chars.SetItem(*adapter, d3n::LookSlot::Torso, piece);
        chars.SetSlotLook(*adapter, d3n::LookSlot::Torso, look);
        REQUIRE(chars.Apply(*adapter));

        const auto after = chars.Slots(*adapter);
        const auto wornTorso = std::find_if(after.begin(), after.end(),
                                            [](const d3p::D3WardrobeSlot& s) {
                                                return s.slot == d3n::LookSlot::Torso;
                                            });
        REQUIRE(wornTorso != after.end());
        CHECK(wornTorso->selectedItem == piece);
        CHECK(wornTorso->lookIndex == look);

        const std::set<u32> torsoGeosets(wornTorso->items[piece].geosets.begin(),
                                         wornTorso->items[piece].geosets.end());
        const auto perGeoset = adapter->GeosetLooks();
        REQUIRE(perGeoset.size() == adapter->EmittedSubObjects().size());
        for (std::size_t g = 0; g < perGeoset.size(); ++g) {
            INFO(stem << " geoset " << g);
            // The override lands on the torso and NOWHERE else — one material
            // serves a whole weight class, so a leak here would silently
            // re-skin the boots and gloves too, which is exactly the bug a
            // model-wide look index cannot avoid.
            if (torsoGeosets.count(static_cast<u32>(g)))
                CHECK(perGeoset[g] == look);
            else
                CHECK(perGeoset[g] == adapter->LookIndex());
        }

        // The widened texture list is a superset of the uniform one, in the
        // same order: the uniform prefix is what every texture id already
        // recorded points into.
        const auto uniform = flakes::io::CollectD3Textures(adapter->SourceAppearance(),
                                                           adapter->LookIndex());
        const auto wide = flakes::io::CollectD3Textures(
            adapter->SourceAppearance(), adapter->LookIndex(), adapter->EmittedSubObjects(),
            perGeoset);
        REQUIRE(wide.size() >= uniform.size());
        for (std::size_t i = 0; i < uniform.size(); ++i)
            CHECK(wide[i].snoId == uniform[i].snoId);
        if (wide.size() > uniform.size())
            ++widened;

        // GetTextures must agree with it, or the surface table's texture ids
        // index a list nobody uploaded.
        const auto staged = adapter->GetTextures();
        REQUIRE(staged.size() == wide.size());
        for (std::size_t i = 0; i < wide.size(); ++i)
            CHECK(staged[i].sharedKey == "#" + std::to_string(wide[i].snoId));
    }
    std::printf("[d3-char] %zu of 14 players' last look added textures the base look lacks\n",
                widened);
}

TEST_CASE("D3 corpus: a model with no wardrobe is left alone", "[d3][character][corpus]") {
    const fs::path dir = CorpusRoot() / "Appearances";
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) {
        WARN("No D3 corpus. SKIPPED, not passed.");
        return;
    }
    // The half no player-model test can see: a rule applied too eagerly strips
    // every creature and prop in the game to nothing, and the D3 corpus is 63%
    // props. Sweep broadly rather than deeply — this is a false-positive count.
    std::vector<fs::path> files;
    for (fs::directory_iterator it(dir, ec), end; it != end; it.increment(ec)) {
        if (ec)
            break;
        if (it->is_regular_file(ec) && it->path().extension() == ".app")
            files.push_back(it->path());
    }
    std::sort(files.begin(), files.end());
    REQUIRE(files.size() > 100);
    // Strided, not the first N. The corpus is 11,347 files and the players sit
    // under B, D, M, W and the x1_/p6_ tail — taking a prefix would sweep six
    // hundred props whose names all begin with A and call that coverage.
    const std::size_t want = std::min<std::size_t>(600, files.size());
    const std::size_t stride = std::max<std::size_t>(1, files.size() / want);

    // The fourteen players go in the same sweep, because "nothing dressed" is
    // what a detector stuck at false reports too. With them present this case
    // says which way each of the two answers went, and a stride that happens
    // to miss every player cannot make it vacuous.
    std::set<fs::path> sample;
    for (std::size_t i = 0; i < files.size(); i += stride)
        sample.insert(files[i]);
    std::set<std::string> wantDressed;
    for (i32 c = 0; c < static_cast<i32>(d3n::kPlayerClassCount); ++c) {
        for (auto g : {d3n::Gender::Male, d3n::Gender::Female}) {
            const std::string stem =
                d3n::playerAppearanceStem(static_cast<d3n::PlayerClass>(c), g);
            const fs::path p = dir / (stem + ".app");
            if (!fs::is_regular_file(p, ec))
                continue;
            sample.insert(p);
            // Canonical case: the shipped stems spell two of the prefixes in a
            // different case from playerAppearanceStem (`x1_`, `p6_`), and this
            // set is compared against what the filesystem hands back.
            wantDressed.insert(fs::canonical(p, ec).stem().string());
        }
    }
    REQUIRE(wantDressed.size() == 14);

    flakes::io::D3SnoCache cache(nullptr);
    d3p::D3CharacterAppearance chars;
    std::size_t built = 0, characters = 0, untouched = 0;
    std::set<std::string> characterNames;

    for (const auto& file : sample) {
        const auto bytes = ReadAll(file);
        auto adapter = flakes::io::D3ModelAdapter::LoadAppearance(
            ContentRef::FromPath(file.string()), bytes, cache);
        if (!adapter)
            continue;
        ++built;
        if (chars.Apply(*adapter)) {
            ++characters;
            characterNames.insert(fs::canonical(file, ec).stem().string());
            continue;
        }
        // Not a character: nothing was set, so every geoset still draws.
        CHECK(adapter->GeosetHidden().empty());
        CHECK(adapter->GeosetLooks().empty());
        ++untouched;
    }

    std::printf("[d3-char] %zu appearance(s): %zu dressed, %zu untouched\n", built, characters,
                untouched);
    for (const auto& n : characterNames)
        std::printf("[d3-char]   dressed: %s\n", n.c_str());
    REQUIRE(built > 0);
    CHECK(built == characters + untouched);
    // Exactly the players, and no one else. Both halves matter: a detector
    // stuck at false passes the second alone, and one stuck at true passes the
    // first.
    CHECK(characterNames == wantDressed);
    // Dressing is the rare case. If this ever inverts, the wardrobe test is
    // matching something every model has rather than something only a player
    // Appearance ships.
    CHECK(characters * 10 < built);
}

// ===========================================================================
// The `.acr` route, on the installed client
// ===========================================================================
//
// Everything above loads an `.app` directly, which is the offline shape and the
// one the render gate uses. A host opens an `.acr` instead: it names the
// appearance, carries the AnimSet, and picks the look. That hop needs the
// storage, so it is gated here rather than there — and it is the route
// Monk_Male.acr and X1_Crusader_Male.acr actually take.

TEST_CASE("D3 install: player actors dress through the `.acr` route",
          "[d3][character][install]") {
    using ::whiteout::flakes::ProductId;

    flakes::io::FileContentProvider provider;
    if (const char* root = std::getenv("WDX_TEST_D3_INSTALL"); root && *root)
        provider.SetInstallPath(root);
    provider.SetGame(ProductId::D3);
    if (provider.GamePath(ProductId::D3).empty()) {
        WARN("No Diablo III install (set WDX_TEST_D3_INSTALL). SKIPPED, not passed.");
        return;
    }
    flakes::io::StorageBrowser browser;
    std::string err;
    if (!browser.Open(provider.GamePath(ProductId::D3), flakes::io::StorageKind::Casc, &err)) {
        WARN("Could not browse the D3 install: " << err << ". SKIPPED, not passed.");
        return;
    }

    // Walked rather than spelled. The D3 root names an id `Base\Actor\<x>.acr`
    // once CoreTOC has been read and `actor\<decimal id>` when it has not, and
    // guessing the first would turn a listing that changed shape into a silent
    // skip. Bounded to two levels, which is where every `.acr` sits.
    std::vector<std::string> actors;
    std::vector<std::pair<std::string, int>> stack{{"", 0}};
    while (!stack.empty()) {
        const auto [dir, depth] = stack.back();
        stack.pop_back();
        browser.NavigateTo(dir);
        if (depth < 2) {
            for (const auto& f : browser.Current().folders)
                stack.push_back({dir.empty() ? f : dir + "\\" + f, depth + 1});
        }
        for (const auto& f : browser.Current().modelFiles) {
            if (f.size() > 4 && f.compare(f.size() - 4, 4, ".acr") == 0)
                actors.push_back(browser.ChildPath(f));
        }
    }
    if (actors.empty()) {
        WARN("No `.acr` in the install listing. SKIPPED, not passed.");
        return;
    }

    // The bare class actors, not the `_FrontEnd` / `_characterSelect` rigs:
    // those are the same appearance and would only re-measure it.
    std::set<std::string> want;
    for (i32 c = 0; c < static_cast<i32>(d3n::kPlayerClassCount); ++c) {
        for (auto g : {d3n::Gender::Male, d3n::Gender::Female}) {
            std::string stem = d3n::playerAppearanceStem(static_cast<d3n::PlayerClass>(c), g);
            std::transform(stem.begin(), stem.end(), stem.begin(),
                           [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
            want.insert(stem + ".acr");
        }
    }

    d3p::D3CharacterAppearance chars;
    std::size_t opened = 0, dressed = 0;
    std::vector<std::string> report;

    for (const std::string& ref : actors) {
        std::string leaf = ref.substr(ref.find_last_of("/\\") + 1);
        std::transform(leaf.begin(), leaf.end(), leaf.begin(),
                       [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        if (!want.count(leaf))
            continue;
        auto bytes = provider.ReadFile(ref);
        if (!bytes)
            continue;
        flakes::io::D3SnoCache cache(&provider);
        auto a = flakes::io::D3ModelAdapter::LoadActor(ContentRef::FromPath(ref), *bytes, cache,
                                                       /*lazyClips=*/true);
        if (!a)
            continue;
        ++opened;
        INFO(ref);
        // The whole point of the `.acr` route: it resolved an appearance, and
        // that appearance is the same dressable thing the `.app` route saw.
        REQUIRE(a->AppearanceSno() > 0);
        REQUIRE(chars.Apply(*a));
        ++dressed;

        const auto slots = chars.Slots(*a);
        REQUIRE(slots.size() >= 4);
        std::size_t shown = 0;
        for (const u8 h : a->GeosetHidden())
            shown += (h == 0) ? 1 : 0;
        CHECK(shown > 0);
        CHECK(shown < a->EmittedSubObjects().size());

        // An Actor picks its own look off a weighted list rather than taking
        // the engine's "A" default, so the slots must start on *that* one — a
        // slot silently reset to 0 would re-skin a `_characterSelect` rig.
        for (const auto& sl : slots) {
            INFO(sl.name);
            CHECK(sl.lookIndex == a->LookIndex());
        }

        std::string line = leaf + ": " + std::to_string(shown) + "/" +
                           std::to_string(a->EmittedSubObjects().size()) + " shown,";
        for (const auto& sl : slots)
            line += " " + sl.name + "=" + sl.items[sl.selectedItem].label + "(" +
                    std::to_string(sl.items.size()) + ")";
        report.push_back(std::move(line));
    }

    for (const auto& r : report)
        std::printf("[d3-char] %s\n", r.c_str());
    if (opened == 0) {
        WARN("No player `.acr` found in the install listing. SKIPPED, not passed.");
        return;
    }
    CHECK(dressed == opened);
    // All fourteen ship as bare class actors. Fewer means the listing changed
    // shape, not that a class stopped being dressable.
    CHECK(opened == 14);
}
