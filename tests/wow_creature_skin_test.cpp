// ============================================================================
// Monster skins — does the client database say what a creature `.m2` won't?
//
// A creature model leaves its skin texture blank on purpose: `M2Texture::type`
// 11..13 are slots the client fills from CreatureDisplayInfo, which is why a
// viewer that opens the file and nothing else draws a white cow. What is
// gated here is the join that fills them — CreatureModelData names the `.m2`
// by fileDataID, and the display rows pointing at it are the skins it can
// wear — and that the resolved id actually reaches the texture slot.
//
// Needs the corpus (`C:/Projects/WhiteoutLib/Corpus/WoW`, override with
// WDX_TEST_WOW_CORPUS), which carries both the models and a `dbfilesclient/`
// dump of the install they came from. Skips when it is absent; skipped is not
// passed.
//
// The fileDataIDs below are fixture data, read out of the community listfile
// for the same install. They are what a listfile-backed storage answers
// FileIdForPath with, and the test stands in for one so the join can be
// checked without a 140 MB CSV.
// ============================================================================

#include <catch2/catch_test_macros.hpp>

#include "io/file_content_provider.h"
#include "io/m2/m2_model_adapter.h"
#include "io/wow/creature_skin_table.h"
#include "renderer/profiles/wow/wow_replaceable_textures.h"
#include "whiteout/flakes/content_ref.h"
#include "whiteout/flakes/util/path_utf8.h"

#include <whiteout/models/m2/m2.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <unordered_map>

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

struct Fixture {
    const char* model;
    whiteout::u32 fileId;
    whiteout::u32 firstDisplayId;
    whiteout::u32 firstSkin; // TextureVariation[0] of that display
};

// The fourth variation slot, which fills texture type 5 rather than 14 — see
// creature_skin_table.h. `sporebat3mount` names all four (body, bodyglow,
// saddle, saddleglow) and is not in the loose `.m2` corpus, so it pins the
// table half only.
constexpr whiteout::u32 kFourSlotModel = 5884327; // creature/sporebat3mount

// Every creature in the `.m2` corpus that declares a replaceable slot.
constexpr Fixture kCreatures[] = {
    {"creature/airelemental/airelemental.m2", 122877, 24622, 122878},
    {"creature/cow/cow.m2", 123288, 1060, 123287},
    {"creature/cryptfiend/cryptfiend.m2", 123329, 3004, 123330},
    {"creature/athissa/athissa.m2", 1247651, 20748, 1247664},
    {"creature/batpet/batpet.m2", 1338861, 68859, 1338862},
    {"creature/aggramar/aggramar.m2", 1599045, 76034, 1599776},
    {"creature/bloodtick/bloodtick.m2", 1616773, 76650, 1616967},
    {"creature/aqir/aqir.m2", 1968538, 85193, 1968541},
    {"creature/crabmount/crabmount.m2", 2395765, 91238, 2819605},
};

/// A storage that knows its own names, which is what a WoW root with a
/// listfile is and a loose directory is not. Also counts client-database
/// reads, so "resolves nothing" can be told apart from "read nothing".
class NamedProvider final : public io::IContentProvider {
public:
    explicit NamedProvider(io::IContentProvider& inner) : inner_(inner) {}

    io::RequestId Request(const ContentRef& ref, io::CompletionCallback cb) override {
        if (!ref.IsFileId() && ref.path.size() > 4 &&
            ref.path.compare(ref.path.size() - 4, 4, ".db2") == 0)
            ++dbReads;
        return inner_.Request(ref, std::move(cb));
    }
    void Wait(io::RequestId id) override {
        inner_.Wait(id);
    }
    void Cancel(io::RequestId id) override {
        inner_.Cancel(id);
    }
    void Pump() override {
        inner_.Pump();
    }
    whiteout::u32 FileIdForPath(const std::string& path) const override {
        const auto it = ids.find(path);
        return it == ids.end() ? 0u : it->second;
    }

    std::unordered_map<std::string, whiteout::u32> ids;
    std::size_t dbReads = 0;

private:
    io::IContentProvider& inner_;
};

std::shared_ptr<io::M2ModelAdapter> LoadModel(io::IContentProvider& provider,
                                              const std::string& rel) {
    auto bytes = provider.ReadFile(rel);
    if (!bytes)
        return nullptr;
    return io::M2ModelAdapter::Load(ContentRef::FromPath(rel),
                                    std::span<const whiteout::u8>(bytes->data(), bytes->size()),
                                    &provider);
}

/// A storage that answers the way a listfile-backed CASC root does: it matches
/// a path by its longest recognised suffix, and it cannot list a directory it
/// was handed absolutely, because no archive entry carries a drive letter.
/// Standing in for a loose extraction, whose folder holds the `.m2` and nothing
/// else at all.
class SuffixProvider final : public io::IContentProvider {
public:
    explicit SuffixProvider(io::IContentProvider& inner) : inner_(inner) {}

    io::RequestId Request(const ContentRef& ref, io::CompletionCallback cb) override {
        return inner_.Request(ref, std::move(cb));
    }
    void Wait(io::RequestId id) override {
        inner_.Wait(id);
    }
    void Cancel(io::RequestId id) override {
        inner_.Cancel(id);
    }
    void Pump() override {
        inner_.Pump();
    }
    whiteout::u32 FileIdForPath(const std::string& path) const override {
        for (std::size_t at = 0; at != std::string::npos;) {
            if (const auto it = ids.find(path.substr(at)); it != ids.end())
                return it->second;
            const auto slash = path.find('/', at);
            at = slash == std::string::npos ? std::string::npos : slash + 1;
        }
        return 0;
    }
    std::vector<std::string> ListFiles(const std::string& directory, bool recursive) override {
        return fs::path(directory).is_absolute() ? std::vector<std::string>()
                                                 : inner_.ListFiles(directory, recursive);
    }

    std::unordered_map<std::string, whiteout::u32> ids;

private:
    io::IContentProvider& inner_;
};

/// How many of a model's textures the client would replace with skin `slot`.
std::size_t SlotsOfType(const whiteout::m2::Model& model, whiteout::u32 type) {
    return static_cast<std::size_t>(
        std::count_if(model.textures.begin(), model.textures.end(),
                      [&](const auto& t) { return t.type == type; }));
}

} // namespace

TEST_CASE("the creature tables name the skin an .m2 leaves blank", "[m2][wow][db2]") {
    std::error_code ec;
    const fs::path root = CorpusRoot();
    if (!fs::is_directory(root / "dbfilesclient", ec)) {
        WARN("WoW corpus dbfilesclient/ not found — skipping");
        return;
    }

    io::FileContentProvider provider;
    provider.SetBasePath(root);

    io::wow::CreatureSkinTable table;
    REQUIRE(table.Load(provider));
    REQUIRE(table.ModelCount() > 0);
    REQUIRE(table.DisplayCount() >= table.ModelCount());

    // fileDataID 0 is CASC's "unset", never a record.
    REQUIRE(table.ForModel(0).empty());

    for (const Fixture& c : kCreatures) {
        INFO(c.model);
        const auto skins = table.ForModel(c.fileId);
        REQUIRE_FALSE(skins.empty());
        // Variation 0 is the lowest display id, so which skin a model wears by
        // default does not depend on how the table happened to be laid out.
        CHECK(skins.front().displayId == c.firstDisplayId);
        CHECK(skins.front().texture[0] == c.firstSkin);
        for (const auto& skin : skins) {
            // A display that fills nothing replaces nothing, so it is not a
            // variation anything should be able to select.
            whiteout::u32 any = 0;
            for (whiteout::u32 s = 0; s < io::wow::kMonsterSkinSlots; ++s)
                any |= skin.texture[s];
            CHECK(any != 0);
        }
        CHECK(std::is_sorted(skins.begin(), skins.end(), [](const auto& a, const auto& b) {
            return a.displayId < b.displayId;
        }));
    }

    // Four slots, not three. Reading only three left `sporebat3mount`'s
    // saddleglow — its type-5 texture — bound to nothing and rendering white.
    REQUIRE(io::wow::kMonsterSkinSlots == 4);
    CHECK(io::wow::kMonsterSkinTypes[3] == 5);
    const auto four = table.ForModel(kFourSlotModel);
    REQUIRE_FALSE(four.empty());
    for (const auto& skin : four)
        CHECK(skin.texture[3] != 0);
}

TEST_CASE("a creature model binds the skin its display names", "[m2][wow][db2]") {
    std::error_code ec;
    const fs::path root = CorpusRoot();
    if (!fs::is_directory(root / "dbfilesclient", ec)) {
        WARN("WoW corpus dbfilesclient/ not found — skipping");
        return;
    }

    io::FileContentProvider backing;
    backing.SetBasePath(root);
    NamedProvider provider(backing);
    for (const Fixture& c : kCreatures)
        provider.ids.emplace(c.model, c.fileId);

    wow::WowReplaceableTextures replaceables;
    replaceables.SetContentProvider(&provider);

    for (const Fixture& c : kCreatures) {
        INFO(c.model);
        if (!fs::exists(root / c.model, ec))
            continue;
        auto adapter = LoadModel(provider, c.model);
        REQUIRE(adapter);

        const std::size_t bound = replaceables.Apply(*adapter, ContentRef::FromPath(c.model));
        REQUIRE_FALSE(replaceables.Variations(ContentRef::FromPath(c.model)).empty());

        const auto& model = adapter->SourceModel();
        const io::wow::MonsterSkin& skin = replaceables.Table().ForModel(c.fileId).front();

        // Every slot the model declares AND the display fills, and no other.
        std::size_t expected = 0;
        for (whiteout::u32 slot = 0; slot < io::wow::kMonsterSkinSlots; ++slot) {
            if (skin.texture[slot] != 0)
                expected += SlotsOfType(model, io::wow::kMonsterSkinTypes[slot]);
        }
        CHECK(bound == expected);
        CHECK(bound > 0);

        // ...and the resolved id is what actually reaches the texture slot.
        const auto textures = adapter->GetTextures();
        REQUIRE(textures.size() == model.textures.size());
        for (std::size_t i = 0; i < textures.size(); ++i) {
            INFO("texture " << i << " type " << model.textures[i].type);
            const whiteout::u32 type = model.textures[i].type;
            const auto* slot = std::find(std::begin(io::wow::kMonsterSkinTypes),
                                         std::end(io::wow::kMonsterSkinTypes), type);
            if (slot == std::end(io::wow::kMonsterSkinTypes))
                continue;
            const whiteout::u32 id = skin.texture[slot - std::begin(io::wow::kMonsterSkinTypes)];
            if (id == 0)
                continue;
            CHECK(textures[i].sharedKey == "#" + std::to_string(id));
        }
    }
}

TEST_CASE("another variation is another skin", "[m2][wow][db2]") {
    std::error_code ec;
    const fs::path root = CorpusRoot();
    if (!fs::is_directory(root / "dbfilesclient", ec)) {
        WARN("WoW corpus dbfilesclient/ not found — skipping");
        return;
    }
    const std::string cow = "creature/cow/cow.m2";
    if (!fs::exists(root / cow, ec)) {
        WARN("creature/cow/cow.m2 not in the corpus — skipping");
        return;
    }

    io::FileContentProvider backing;
    backing.SetBasePath(root);
    NamedProvider provider(backing);
    provider.ids.emplace(cow, 123288u);

    wow::WowReplaceableTextures replaceables;
    replaceables.SetContentProvider(&provider);

    auto first = LoadModel(provider, cow);
    REQUIRE(first);
    REQUIRE(replaceables.Apply(*first, ContentRef::FromPath(cow)) > 0);

    // The cow's ten displays wear two skins between them, so at least one
    // variation is a different texture. Found rather than named: which index
    // that is belongs to the table, not to this test.
    //
    // Searched in the offered list, not in the table behind it — the two are
    // no longer parallel now that repeated looks collapse, and SetVariation
    // indexes what a picker shows.
    const auto& offered = replaceables.Variations(ContentRef::FromPath(cow));
    REQUIRE(offered.size() > 1);
    const auto other = std::find_if(offered.begin(), offered.end(), [&](const auto& s) {
        return s.texture[0] != offered.front().texture[0];
    });
    REQUIRE(other != offered.end());
    const std::string wanted = other->texture[0];

    replaceables.SetVariation(static_cast<whiteout::u32>(other - offered.begin()));
    auto second = LoadModel(provider, cow);
    REQUIRE(second);
    REQUIRE(replaceables.Apply(*second, ContentRef::FromPath(cow)) > 0);

    const auto before = first->GetTextures();
    const auto after = second->GetTextures();
    REQUIRE(before.size() == after.size());
    bool differs = false;
    for (std::size_t i = 0; i < before.size(); ++i)
        differs = differs || before[i].sharedKey != after[i].sharedKey;
    CHECK(differs);
    CHECK(after[0].sharedKey == wanted);
}

TEST_CASE("the skins offered are the distinct ones", "[m2][wow][db2]") {
    std::error_code ec;
    const fs::path root = CorpusRoot();
    if (!fs::is_directory(root / "dbfilesclient", ec)) {
        WARN("WoW corpus dbfilesclient/ not found — skipping");
        return;
    }

    io::FileContentProvider backing;
    backing.SetBasePath(root);
    NamedProvider provider(backing);
    for (const Fixture& c : kCreatures)
        provider.ids.emplace(c.model, c.fileId);

    wow::WowReplaceableTextures replaceables;
    replaceables.SetContentProvider(&provider);

    // A creature model is named by every display record that uses it, and those
    // differ in far more than the model wears. The cryptfiend's 21 displays are
    // three pictures; a picker offering 21 offers the same one nineteen times.
    std::size_t collapsed = 0;
    for (const Fixture& c : kCreatures) {
        INFO(c.model);
        if (!fs::exists(root / c.model, ec))
            continue;
        auto adapter = LoadModel(provider, c.model);
        REQUIRE(adapter);
        REQUIRE(replaceables.Apply(*adapter, ContentRef::FromPath(c.model)) > 0);

        const auto& offered = replaceables.Variations(ContentRef::FromPath(c.model));
        REQUIRE_FALSE(offered.empty());
        for (std::size_t i = 0; i < offered.size(); ++i) {
            for (std::size_t j = i + 1; j < offered.size(); ++j) {
                INFO("variations " << i << " and " << j << " are the same skin");
                CHECK_FALSE(std::equal(std::begin(offered[i].texture), std::end(offered[i].texture),
                                       std::begin(offered[j].texture)));
            }
        }
        // Still the client's own answer, and still in display order: dropping a
        // repeat must not disturb which skin a model wears by default.
        CHECK(offered.front().texture[0] == "#" + std::to_string(c.firstSkin));

        const auto rows = replaceables.Table().ForModel(c.fileId).size();
        CHECK(offered.size() <= rows);
        if (offered.size() < rows)
            ++collapsed;
    }
    // The premise: this is not a rule with no cases. cryptfiend alone is 21→3.
    CHECK(collapsed > 0);
}

TEST_CASE("without a listfile the skins are the model's own siblings", "[m2][wow][db2]") {
    std::error_code ec;
    const fs::path root = CorpusRoot();
    const std::string aqir = "creature/aqir/aqir.m2";
    if (!fs::exists(root / aqir, ec)) {
        WARN("creature/aqir/aqir.m2 not in the corpus — skipping");
        return;
    }

    // The plain provider: a loose tree answers FileIdForPath with 0, which is
    // what a CASC root with no listfile does too. Nothing can join the client
    // databases, and a creature would render white if that were the only route.
    io::FileContentProvider provider;
    provider.SetBasePath(root);

    wow::WowReplaceableTextures replaceables;
    replaceables.SetContentProvider(&provider);
    REQUIRE(provider.FileIdForPath(aqir) == 0);

    auto adapter = LoadModel(provider, aqir);
    REQUIRE(adapter);
    CHECK(replaceables.Apply(*adapter, ContentRef::FromPath(aqir)) == 1);

    // Named after the model, so the effect and reflection textures creatures
    // share a folder with are not offered as skins.
    const auto& skins = replaceables.Variations(ContentRef::FromPath(aqir));
    REQUIRE(skins.size() == 4);
    CHECK(skins.front().label == "aqir");
    CHECK(skins.front().texture[0] == "creature/aqir/aqir.blp");
    CHECK(adapter->GetTextures().front().sharedKey == "creature/aqir/aqir.blp");

    // ...and every one of them is a file the same provider can read, which is
    // the difference from a fileDataID nothing on disk resolves.
    for (const auto& skin : skins) {
        INFO(skin.label);
        CHECK(provider.ReadFile(skin.texture[0]).has_value());
    }

    replaceables.SetVariation(1);
    auto second = LoadModel(provider, aqir);
    REQUIRE(second);
    REQUIRE(replaceables.Apply(*second, ContentRef::FromPath(aqir)) == 1);
    CHECK(second->GetTextures().front().sharedKey == skins[1].texture[0]);
}

TEST_CASE("a skin fills every slot the model declares", "[m2][wow][db2]") {
    std::error_code ec;
    const fs::path root = CorpusRoot();
    if (!fs::is_directory(root / "dbfilesclient", ec)) {
        WARN("WoW corpus dbfilesclient/ not found — skipping");
        return;
    }
    const std::string crab = "creature/crabmount/crabmount.m2";
    if (!fs::exists(root / crab, ec)) {
        WARN("creature/crabmount/crabmount.m2 not in the corpus — skipping");
        return;
    }

    // A mount is the case a one-file-per-skin picker gets wrong: the model
    // declares types 11 *and* 12, CreatureDisplayInfo fills both from one
    // display, and offering the two files separately puts the saddle texture on
    // the crab's body. Both routes have to pair them.
    io::FileContentProvider backing;
    backing.SetBasePath(root);

    SECTION("from the display record") {
        NamedProvider provider(backing);
        provider.ids.emplace(crab, 2395765u);
        wow::WowReplaceableTextures replaceables;
        replaceables.SetContentProvider(&provider);

        auto adapter = LoadModel(provider, crab);
        REQUIRE(adapter);
        CHECK(replaceables.Apply(*adapter, ContentRef::FromPath(crab)) == 2);

        const auto& skins = replaceables.Variations(ContentRef::FromPath(crab));
        REQUIRE(skins.size() >= 2);
        // 2819605 = crabmount_body, 2819604 = crabmount_saddle. Slot order is
        // the display's, not the ids' — the saddle sorts first by id and second
        // by slot, which is what makes this worth asserting.
        CHECK(skins.front().texture[0] == "#2819605");
        CHECK(skins.front().texture[1] == "#2819604");
    }

    SECTION("from the siblings, paired by name") {
        wow::WowReplaceableTextures replaceables;
        replaceables.SetContentProvider(&backing);
        REQUIRE(backing.FileIdForPath(crab) == 0); // the premise: no identity

        auto adapter = LoadModel(backing, crab);
        REQUIRE(adapter);
        CHECK(replaceables.Apply(*adapter, ContentRef::FromPath(crab)) == 2);

        const auto& skins = replaceables.Variations(ContentRef::FromPath(crab));
        REQUIRE(skins.size() == 2);
        CHECK(skins[0].label == "crabmount_blue");
        CHECK(skins[0].texture[0] == "creature/crabmount/crabmount_blue_body.blp");
        CHECK(skins[0].texture[1] == "creature/crabmount/crabmount_blue_saddle.blp");
        CHECK(skins[1].label == "crabmount");
        CHECK(skins[1].texture[0] == "creature/crabmount/crabmount_body.blp");
        CHECK(skins[1].texture[1] == "creature/crabmount/crabmount_saddle.blp");
    }
}

TEST_CASE("names that do not say how to pair are not paired", "[m2][wow][db2]") {
    std::error_code ec;
    const fs::path root = CorpusRoot();
    const std::string bat = "creature/batpet/batpet.m2";
    if (!fs::exists(root / bat, ec)) {
        WARN("creature/batpet/batpet.m2 not in the corpus — skipping");
        return;
    }

    // The counter-example that keeps the pairing rule honest. batpet declares
    // two slots and its folder holds six skins, but the client pairs `batpet`
    // with `batpetglow` and `batpetfire` with `batpetglowfire` — a grouping no
    // ordering of those names produces. So the sibling route must decline to
    // pair rather than invent one, and leave the second slot alone.
    io::FileContentProvider provider;
    provider.SetBasePath(root);
    wow::WowReplaceableTextures replaceables;
    replaceables.SetContentProvider(&provider);

    auto adapter = LoadModel(provider, bat);
    REQUIRE(adapter);
    CHECK(replaceables.Apply(*adapter, ContentRef::FromPath(bat)) == 1);

    for (const auto& skin : replaceables.Variations(ContentRef::FromPath(bat))) {
        INFO(skin.label);
        CHECK_FALSE(skin.texture[0].empty());
        CHECK(skin.texture[1].empty());
    }
}

TEST_CASE("a model with no replaceable slot reads no client database", "[m2][wow][db2]") {
    std::error_code ec;
    const fs::path root = CorpusRoot();
    const std::string plain = "creature/alexstrasza/alexstrasza.m2";
    if (!fs::exists(root / plain, ec)) {
        WARN("creature/alexstrasza/alexstrasza.m2 not in the corpus — skipping");
        return;
    }

    io::FileContentProvider backing;
    backing.SetBasePath(root);
    NamedProvider provider(backing);
    provider.ids.emplace(plain, 234497u);

    wow::WowReplaceableTextures replaceables;
    replaceables.SetContentProvider(&provider);

    auto adapter = LoadModel(provider, plain);
    REQUIRE(adapter);
    for (const auto& tex : adapter->SourceModel().textures)
        REQUIRE(tex.type == 0); // the premise: nothing here is replaceable

    CHECK(replaceables.Apply(*adapter, ContentRef::FromPath(plain)) == 0);
    // The point of the check: eight megabytes of client database is not the
    // price of opening a model that has no slot to fill.
    CHECK(provider.dbReads == 0);
}

TEST_CASE("a model opened absolutely still finds the storage's folder", "[m2][wow][db2]") {
    std::error_code ec;
    const fs::path root = CorpusRoot();
    const std::string cow = "creature/cow/cow.m2";
    if (!fs::exists(root / cow, ec)) {
        WARN("creature/cow/cow.m2 not in the corpus — skipping");
        return;
    }

    io::FileContentProvider backing;
    backing.SetBasePath(root);
    SuffixProvider provider(backing);
    // An id no display row names, so the `.blp` beside the model is the only
    // answer left — `revenantair` is the shipped case, with five skins in the
    // storage and not one display record pointing at the model.
    provider.ids.emplace(cow, 1u);

    wow::WowReplaceableTextures replaceables;
    replaceables.SetContentProvider(&provider);

    // What a host's file dialog hands back for a file on disk. The folder it
    // names holds the `.m2` alone; the folder the storage knows holds the
    // skins, and before this both `revenantair` and `sporebat3mountglowing`
    // rendered white because only the first was ever asked.
    std::string absolute = whiteout::flakes::io::PathToUtf8(root / cow);
    std::replace(absolute.begin(), absolute.end(), '\\', '/');
    const ContentRef ref = ContentRef::FromPath(absolute);

    auto adapter = LoadModel(provider, cow);
    REQUIRE(adapter);
    CHECK(replaceables.Apply(*adapter, ref) > 0);

    const auto& offered = replaceables.Variations(ref);
    REQUIRE_FALSE(offered.empty());
    for (const auto& v : offered)
        CHECK(v.texture[0].rfind("creature/cow/", 0) == 0);
}

TEST_CASE("the creature tables load from a bare CASC root", "[m2][wow][db2]") {
    // The regression this exists for. `creaturedisplayinfo.db2` ships a
    // TACT-locked frame on a stock 11.x install, and a single such frame fails
    // the whole read — so with zero-fill off the table came back *missing*,
    // CreatureSkinTable::Load returned false, and every creature whose skin
    // lives in TextureVariation bound white. It looked like a stale listfile
    // and was not: the file is unreadable by path and by id alike.
    //
    // Bare on purpose — no listfile, no key list. Those are the two things a
    // user is most likely not to have, and neither should be what stands
    // between a creature and its skin: zero-fill salvages the encrypted table,
    // and the hardcoded fileDataIDs reach it without a name.
    io::FileContentProvider provider;
    provider.SetGame(whiteout::flakes::ProductId::Wow);
    if (provider.GamePath(whiteout::flakes::ProductId::Wow).empty() || !provider.HasCasc())
        SKIP("no World of Warcraft CASC install found");
    REQUIRE_FALSE(provider.HasListfile());

    io::wow::CreatureSkinTable table;
    REQUIRE(table.Load(provider));
    // Thousands, not a handful: a table that parsed but joined nothing would
    // satisfy a bare `Loaded()` and still leave every creature white.
    CHECK(table.ModelCount() > 1000);
    CHECK(table.DisplayCount() > table.ModelCount());
}

TEST_CASE("a creature skins itself from a bare CASC root", "[m2][wow][db2]") {
    // The same claim one layer up, through the adapter that actually binds the
    // slots. `centaur2_male` declares types 11 and 12 and names neither — its
    // TXID carries 0 for both — so a filled count of 2 is the whole feature
    // working end to end off nothing but an install.
    constexpr whiteout::u32 kCentaur2Male = 4036647;

    io::FileContentProvider provider;
    provider.SetGame(whiteout::flakes::ProductId::Wow);
    if (provider.GamePath(whiteout::flakes::ProductId::Wow).empty() || !provider.HasCasc())
        SKIP("no World of Warcraft CASC install found");

    const ContentRef ref = ContentRef::FromFileId(kCentaur2Male);
    auto bytes = provider.ReadFile(ref);
    if (!bytes || bytes->empty())
        SKIP("creature/centaur2_male is not in this build of the install");

    auto adapter = io::M2ModelAdapter::Load(
        ref, std::span<const whiteout::u8>(bytes->data(), bytes->size()), &provider);
    REQUIRE(adapter);
    REQUIRE(SlotsOfType(adapter->SourceModel(), 11) == 1); // the premise
    REQUIRE(SlotsOfType(adapter->SourceModel(), 12) == 1);

    wow::WowReplaceableTextures replaceables;
    replaceables.SetContentProvider(&provider);
    CHECK(replaceables.Apply(*adapter, ref) == 2);

    // And the ids it chose are readable — a slot filled with an id nothing can
    // produce renders exactly as white as one that was never filled.
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
    CHECK(named == 4); // two skin slots + the two the model names in TXID
}
