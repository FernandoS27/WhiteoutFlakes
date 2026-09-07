// ============================================================================
// Sc2ModelCatalog — the `.m3` -> `.m3a` join StarCraft II and Heroes of the
// Storm spell in their GameData `CModel` entries.
//
// Two halves, for the reason content_provider_games_test has two: the rules are
// asserted against a synthetic catalog that ships in this file, and the CLAIMS
// about the shipped data — 1,331 Heroes models name an animation file, the
// parent chain carries a fifth of them, `index=` replaces rather than appends —
// are measured against a real install when one is there and skipped loudly when
// it is not. Skipped is not passed.
//
// Device-free: a catalog is XML and a hash map.
// ============================================================================

#include <catch2/catch_test_macros.hpp>

#include "io/file_content_provider.h"
#include "renderer/profiles/sc2_heroes/sc2_model_catalog.h"
#include "whiteout/flakes/content_provider.h"
#include "whiteout/flakes/content_ref.h"

#include <algorithm>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

using whiteout::flakes::ContentRef;
using whiteout::flakes::ProductId;
using whiteout::flakes::io::FileContentProvider;
using whiteout::flakes::renderer::profiles::sc2_heroes::Sc2ModelCatalog;
using whiteout::u8;

namespace {

// A provider over a literal map. Enough for the catalog, which only ever asks
// for a listing and then reads what the listing named.
class MapProvider final : public whiteout::flakes::io::IContentProvider {
public:
    std::map<std::string, std::string> files;

    whiteout::flakes::io::RequestId Request(
        const ContentRef& ref, whiteout::flakes::io::CompletionCallback cb) override {
        whiteout::flakes::io::RequestResult r;
        const auto it = files.find(Lower(ref.path));
        if (it != files.end()) {
            r.ok = true;
            r.data.assign(it->second.begin(), it->second.end());
        }
        cb(std::move(r));
        return 1;
    }
    void Wait(whiteout::flakes::io::RequestId) override {}
    void Cancel(whiteout::flakes::io::RequestId) override {}
    void Pump() override {}

    std::vector<std::string> ListFiles(const std::string&, bool) override {
        std::vector<std::string> out;
        for (const auto& [k, v] : files)
            out.push_back(k);
        return out;
    }

    static std::string Lower(std::string s) {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c == '\\' ? '/' : c));
        });
        return s;
    }
};

// The four shapes the shipped catalog uses, in one file: a plain entry, an
// abstract parent that carries animations but no model of its own, a child that
// overrides one slot by index, and the `RequiredAnimsEx`/`FilePath` spelling.
constexpr const char* kCatalog = R"(<?xml version="1.0" encoding="us-ascii"?>
<Catalog>
  <CModel default="1" id="HeroModelParent"/>
  <CModel id="HeroZagara" parent="HeroModelParent" Race="Zerg">
    <Model value="Assets\Units\Heroes\Storm_Hero_Zagara_Base\Storm_Hero_Zagara_Base.m3"/>
    <RequiredAnims value="Assets\Units\Heroes\Zagara_RequiredAnims\Zagara_RequiredAnims.m3a"/>
    <RequiredAnims value="Assets\Portraits\Zagara_PortraitAnims\Zagara_PortraitAnims.m3a"/>
  </CModel>
  <CModel id="HeroMuradinCommon" parent="HeroModelParent">
    <RequiredAnims value="Assets\Units\Heroes\Muradin_RequiredAnims\Muradin_RequiredAnims.m3a"/>
    <RequiredAnims value="Assets\Portraits\Muradin_PortraitAnims\Muradin_PortraitAnims.m3a"/>
    <RequiredAnims value="Assets\Units\Heroes\Muradin_FacialAnims\Muradin_FacialAnims.m3a"/>
  </CModel>
  <CModel id="HeroMuradin" parent="HeroMuradinCommon">
    <Model value="Assets\Units\Heroes\Storm_Hero_Muradin_Base\Storm_Hero_Muradin_Base.m3"/>
  </CModel>
  <CModel id="HeroMuradinMarauder" parent="HeroMuradinCommon">
    <Model value="Assets\Units\Heroes\Storm_Hero_Muradin_Marauder\Storm_Hero_Muradin_Marauder.m3"/>
    <RequiredAnims index="0" value="Assets\Units\Heroes\Marauder_RequiredAnims\Marauder_RequiredAnims.m3a"/>
  </CModel>
  <CModel id="HeroMuradinSilent" parent="HeroMuradinCommon">
    <Model value="Assets\Units\Heroes\Storm_Hero_Muradin_Silent\Storm_Hero_Muradin_Silent.m3"/>
    <RequiredAnims index="2" removed="1"/>
  </CModel>
  <CModel id="HeroAbathur" parent="HeroModelParent">
    <Model value="Assets\Units\Heroes\Storm_Hero_Abathur_Base\Storm_Hero_Abathur_Base.m3"/>
    <RequiredAnimsEx FilePath="Assets\Units\Heroes\Abathur_RequiredAnims\Abathur_RequiredAnims.m3a">
    </RequiredAnimsEx>
  </CModel>
  <CModel id="AbstractTemplate" parent="HeroModelParent">
    <Model value="Assets\Units\##race##\##id##\##id##.m3"/>
    <RequiredAnims value="Assets\Units\Heroes\Token_RequiredAnims\Token_RequiredAnims.m3a"/>
  </CModel>
  <CModel id="NoAnimsAtAll" parent="HeroModelParent">
    <Model value="Assets\Effects\Storm_Effect_Plain\Storm_Effect_Plain.m3"/>
    <LowQualityModel value="SomethingElse"/>
  </CModel>
</Catalog>
)";

MapProvider MakeProvider() {
    MapProvider p;
    p.files["mods/heroesdata.stormmod/base.stormdata/gamedata/modeldata.xml"] = kCatalog;
    return p;
}

std::string Joined(const std::vector<std::string>& v) {
    std::string s;
    for (const auto& e : v)
        s += (s.empty() ? "" : ", ") + e;
    return s;
}

} // namespace

TEST_CASE("A CModel's RequiredAnims name the model's .m3a", "[sc2catalog]") {
    MapProvider provider = MakeProvider();
    Sc2ModelCatalog catalog;
    catalog.SetContentProvider(&provider);
    REQUIRE(catalog.Prewarm());
    CHECK(catalog.Loaded());

    // Keyed on the `assets/...` tail, so the full storage path a browser hands
    // over finds the entry the catalog wrote mod-relative.
    const auto& zagara = catalog.AnimationsFor(ContentRef::FromPath(
        "mods/heroes.stormmod/base.stormassets/assets/units/heroes/"
        "storm_hero_zagara_base/storm_hero_zagara_base.m3"));
    REQUIRE(zagara.size() == 2);
    CHECK(zagara[0] == "assets/units/heroes/zagara_requiredanims/zagara_requiredanims.m3a");
    CHECK(zagara[1] == "assets/portraits/zagara_portraitanims/zagara_portraitanims.m3a");

    // The same entry reached the other two ways a model arrives: the catalog's
    // own relative spelling, and a bare filename out of a flat extraction.
    CHECK(catalog.AnimationsFor(ContentRef::FromPath(
              "Assets\\Units\\Heroes\\Storm_Hero_Zagara_Base\\Storm_Hero_Zagara_Base.m3")) ==
          zagara);
    CHECK(catalog.AnimationsFor(ContentRef::FromPath("Storm_Hero_Zagara_Base.m3")) == zagara);
}

TEST_CASE("parent= carries animations to a child that declares no Model", "[sc2catalog]") {
    MapProvider provider = MakeProvider();
    Sc2ModelCatalog catalog;
    catalog.SetContentProvider(&provider);
    REQUIRE(catalog.Prewarm());

    // HeroMuradinCommon has the three files and no <Model>; HeroMuradin has the
    // model and no files. Neither is usable alone — 110 of the 881 shipped
    // entries that carry RequiredAnims are the first kind.
    const auto& base = catalog.AnimationsFor(ContentRef::FromPath("Storm_Hero_Muradin_Base.m3"));
    REQUIRE(base.size() == 3);
    CHECK(base[0] == "assets/units/heroes/muradin_requiredanims/muradin_requiredanims.m3a");
    CHECK(base[2] == "assets/units/heroes/muradin_facialanims/muradin_facialanims.m3a");
}

TEST_CASE("index= replaces an inherited slot, it does not append", "[sc2catalog]") {
    MapProvider provider = MakeProvider();
    Sc2ModelCatalog catalog;
    catalog.SetContentProvider(&provider);
    REQUIRE(catalog.Prewarm());

    // The Marauder skin overrides slot 0 only: its own body animation, and the
    // portrait and facial sets it shares with every other Muradin. Appending
    // would give it two Muradin bodies, which is the defect this pins.
    const auto& marauder =
        catalog.AnimationsFor(ContentRef::FromPath("Storm_Hero_Muradin_Marauder.m3"));
    REQUIRE(marauder.size() == 3);
    CHECK(marauder[0] == "assets/units/heroes/marauder_requiredanims/marauder_requiredanims.m3a");
    CHECK(marauder[1] == "assets/portraits/muradin_portraitanims/muradin_portraitanims.m3a");
    CHECK(marauder[2] == "assets/units/heroes/muradin_facialanims/muradin_facialanims.m3a");
    CHECK(std::find(marauder.begin(), marauder.end(),
                    "assets/units/heroes/muradin_requiredanims/muradin_requiredanims.m3a") ==
          marauder.end());

    // `removed="1"` clears the slot rather than leaving the parent's file in it.
    const auto& silent =
        catalog.AnimationsFor(ContentRef::FromPath("Storm_Hero_Muradin_Silent.m3"));
    REQUIRE(silent.size() == 2);
    CHECK(std::find(silent.begin(), silent.end(),
                    "assets/units/heroes/muradin_facialanims/muradin_facialanims.m3a") ==
          silent.end());
}

TEST_CASE("RequiredAnimsEx is the same field, and tokens are not a model",
          "[sc2catalog]") {
    MapProvider provider = MakeProvider();
    Sc2ModelCatalog catalog;
    catalog.SetContentProvider(&provider);
    REQUIRE(catalog.Prewarm());

    const auto& abathur = catalog.AnimationsFor(ContentRef::FromPath("Storm_Hero_Abathur_Base.m3"));
    REQUIRE(abathur.size() == 1);
    CHECK(abathur[0] == "assets/units/heroes/abathur_requiredanims/abathur_requiredanims.m3a");

    // A `##token##` path is an abstract template its children expand. Left in,
    // it collects every skin's animations onto a key no file can ever match.
    CHECK(catalog.AnimationsFor(ContentRef::FromPath("assets/units/##race##/##id##/##id##.m3"))
              .empty());

    // A model with no RequiredAnims is not in the index at all, so Apply reads
    // nothing for it — which is most models.
    CHECK(catalog.AnimationsFor(ContentRef::FromPath("Storm_Effect_Plain.m3")).empty());
    // `<LowQualityModel>` must not be read as this entry's `<Model>`.
    CHECK(catalog.AnimationsFor(ContentRef::FromPath("SomethingElse")).empty());
}

TEST_CASE("A catalog with no GameData in it is a normal state", "[sc2catalog]") {
    MapProvider provider; // a Warcraft III storage: models, no catalog
    provider.files["units/human/footman/footman.mdx"] = "not xml";
    Sc2ModelCatalog catalog;
    catalog.SetContentProvider(&provider);
    CHECK_FALSE(catalog.Prewarm());
    CHECK(catalog.ModelCount() == 0);
    CHECK(catalog.AnimationsFor(ContentRef::FromPath("footman.mdx")).empty());
}

TEST_CASE("Changing the install drops the index", "[sc2catalog]") {
    MapProvider a = MakeProvider();
    MapProvider b;
    Sc2ModelCatalog catalog;
    catalog.SetContentProvider(&a);
    REQUIRE(catalog.Prewarm());
    REQUIRE(catalog.ModelCount() > 0);

    catalog.SetContentProvider(&b);
    CHECK_FALSE(catalog.Loaded());
    CHECK(catalog.ModelCount() == 0);
}

// ---------------------------------------------------------------------------
// The shipped catalogs
// ---------------------------------------------------------------------------

namespace {

// Reads the real catalog out of an installed game. Returns nullptr when that
// game is not installed, which is the CI case.
struct InstalledGame {
    std::unique_ptr<FileContentProvider> provider;
    std::string root;
};

InstalledGame OpenSc2Install(bool heroes) {
    InstalledGame g;
    auto p = std::make_unique<FileContentProvider>();
    p->SetGame(ProductId::Sc2);
    const std::string root = heroes ? p->HotsPath() : p->GamePath(ProductId::Sc2);
    if (root.empty())
        return g;
    // One install at a time. `ProductId::Sc2` covers two roots and opens both,
    // and an empty Heroes override reverts to the DISCOVERED path — so leaving
    // it blank measures StarCraft II's catalog plus Heroes' and calls the total
    // StarCraft II's. Pointing both at one root is what isolates it; the CASC
    // registry hands back the same storage for both, so this costs nothing.
    p->SetInstallPath(root);
    p->SetHotsInstallPath(root);
    g.root = root;
    g.provider = std::move(p);
    return g;
}

} // namespace

TEST_CASE("One provider repointed at another install is not the same catalog",
          "[sc2catalog][install]") {
    InstalledGame heroes = OpenSc2Install(/*heroes*/ true);
    InstalledGame sc2 = OpenSc2Install(/*heroes*/ false);
    if (!heroes.provider || !sc2.provider)
        SKIP("needs BOTH StarCraft II and Heroes of the Storm installed");

    // A host keeps ONE provider and repoints it — the viewer's settings page
    // switches product and install path on the provider it already has. The
    // pointer does not move, so a catalog that keys on the pointer keeps
    // serving the install it read first, and every model of the new one gets
    // the old one's animations or none at all.
    auto& p = *heroes.provider;
    Sc2ModelCatalog catalog;
    catalog.SetContentProvider(&p);
    REQUIRE(catalog.Prewarm());
    const std::size_t asHeroes = catalog.ModelCount();
    REQUIRE(asHeroes > 0);

    p.SetInstallPath(sc2.root);
    p.SetHotsInstallPath(sc2.root);
    catalog.SetContentProvider(&p); // same pointer, different install
    CHECK_FALSE(catalog.Loaded());
    REQUIRE(catalog.Prewarm());
    CHECK(catalog.ModelCount() != asHeroes);

    // And back: the index built for Heroes is adopted rather than re-read,
    // which is what keeps the Storage Explorer and the host's documents from
    // rebuilding it at each other every time the user looks away from the grid.
    p.SetInstallPath(heroes.root);
    p.SetHotsInstallPath(heroes.root);
    catalog.SetContentProvider(&p);
    CHECK(catalog.Loaded());
    CHECK(catalog.ModelCount() == asHeroes);
}

TEST_CASE("The shipped Heroes catalog names the models it is claimed to", "[sc2catalog][install]") {
    InstalledGame g = OpenSc2Install(/*heroes*/ true);
    if (!g.provider)
        SKIP("no Heroes of the Storm install found");

    Sc2ModelCatalog catalog;
    catalog.SetContentProvider(g.provider.get());
    REQUIRE(catalog.Prewarm());

    std::printf("[sc2catalog] Heroes '%s': %zu models with external animations\n", g.root.c_str(),
                catalog.ModelCount());

    // Measured over the shipped catalog: 1,343 distinct model paths, of which
    // 1,331 name a file that is still in CASC. The basename fold adds one key
    // per model on top. A floor rather than an equality — content patches move
    // it — but a floor an empty or half-parsed index cannot clear.
    CHECK(catalog.ModelCount() >= 2000);

    // The two hero entries the unit cases above are modelled on, as shipped.
    const auto& zagara = catalog.AnimationsFor(
        ContentRef::FromPath("assets/units/heroes/storm_hero_zagara_base/"
                             "storm_hero_zagara_base.m3"));
    INFO("Zagara -> " << Joined(zagara));
    REQUIRE(zagara.size() == 2);
    CHECK(zagara[0].find("storm_hero_zagara_requiredanims") != std::string::npos);
    CHECK(zagara[1].find("storm_hero_zagara_portraitanims") != std::string::npos);

    // Every file the catalog names must be readable through the provider as it
    // stands — that is the claim that the relative `assets\` path needs no
    // resolution of its own (CascSource's asset-prefix retry does it).
    for (const std::string& rel : zagara) {
        const auto bytes = g.provider->ReadFile(ContentRef::FromPath(rel));
        INFO("reading " << rel);
        REQUIRE(bytes.has_value());
        REQUIRE(bytes->size() > 4);
        // An `.m3a` is an ordinary MD34 container.
        CHECK(std::string(reinterpret_cast<const char*>(bytes->data()), 4) == "43DM");
    }
}

TEST_CASE("StarCraft II spells the same field, and is measured separately",
          "[sc2catalog][install]") {
    InstalledGame g = OpenSc2Install(/*heroes*/ false);
    if (!g.provider)
        SKIP("no StarCraft II install found");

    Sc2ModelCatalog catalog;
    catalog.SetContentProvider(g.provider.get());
    REQUIRE(catalog.Prewarm());

    std::printf("[sc2catalog] StarCraft II '%s': %zu models with external animations\n",
                g.root.c_str(), catalog.ModelCount());

    // 354 model paths, 333 of them still in CASC — an order of magnitude below
    // Heroes, which is the point of measuring them apart.
    CHECK(catalog.ModelCount() >= 500);
}
