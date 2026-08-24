// ============================================================================
// FileContentProvider across the three products it now serves.
//
// Warcraft III, World of Warcraft and StarCraft II / Heroes have genuinely
// different storage rules — a mod-prefix TVFS chain plus three MPQs, an
// id-keyed root with a version-dependent Data/ MPQ set, and CASC-only with two
// installs behind one ProductId. The parts that are pure policy (which MPQ
// names, which product maps where) are asserted unconditionally; the parts that
// need a real install are asserted only when the game finder found one, and
// skipped loudly otherwise. Skipped is not passed.
//
// Device-free — a content provider is files and threads, no GPU — so this is a
// G0 test and runs in CI, where it will skip every install-dependent section.
// ============================================================================

#include <catch2/catch_test_macros.hpp>

#include "io/file_content_provider.h"
#include "io/storage/casc_registry.h"
#include "io/storage_browser.h"
#include "whiteout/flakes/content_ref.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

using whiteout::flakes::ProductId;
using whiteout::flakes::io::FileContentProvider;
using whiteout::flakes::io::OpenCascCount;
using whiteout::flakes::io::StorageBrowser;
using whiteout::flakes::io::StorageKind;

TEST_CASE("MPQ defaults are per-product", "[provider]") {
    // Warcraft III's three archives, and the no-argument overload still
    // meaning exactly that — every existing caller relies on it.
    const auto wc3 = FileContentProvider::DefaultMpqList();
    REQUIRE(wc3.size() == 3);
    CHECK(wc3[0] == "War3Patch.mpq");
    CHECK(wc3 == FileContentProvider::DefaultMpqList(ProductId::Wc3));

    // StarCraft II and Heroes never shipped an MPQ. An empty list is the
    // claim, not an oversight.
    CHECK(FileContentProvider::DefaultMpqList(ProductId::Sc2).empty());

    // WoW's are under Data/, unlike WC3's which sit at the install root.
    const auto wow = FileContentProvider::DefaultMpqList(ProductId::Wow);
    REQUIRE_FALSE(wow.empty());
    for (const auto& n : wow)
        CHECK(n.rfind("Data/", 0) == 0);
}

TEST_CASE("Storages open on demand, not on configuration", "[provider]") {
    FileContentProvider p;
    // Construction discovers installs but opens nothing: a host that creates a
    // scene it never reads from (a thumbnail cell, a settings page being
    // clicked through) must not pay for parsing a CASC index.
    CHECK(p.StoragesPending());

    // Reconfiguration is still just configuration. Three games in a settings
    // panel would otherwise mean three CASC opens for the one the user reads.
    p.SetGame(ProductId::Wow);
    p.SetGame(ProductId::Sc2);
    p.SetGame(ProductId::Wc3);
    p.SetIgnoreMpq(true);
    p.SetMpqList({});
    CHECK(p.StoragesPending());

    // Asking a question only an open storage can answer is a demand, and pays
    // for it once.
    (void)p.HasCasc();
    CHECK_FALSE(p.StoragesPending());
}

TEST_CASE("A visited game keeps its storages across a switch", "[provider]") {
    FileContentProvider p;
    (void)p.HasCasc(); // demands the Warcraft III open
    REQUIRE_FALSE(p.StoragesPending());

    p.SetGame(ProductId::Wow);
    CHECK(p.StoragesPending()); // nothing has read WoW content yet

    // The point of the per-product slots: coming back finds what Warcraft III
    // opened still open. Rebuilding it would mean parsing a CASC index again
    // for every trip through the settings panel, which is the cost that made
    // switching games visibly stall.
    p.SetGame(ProductId::Wc3);
    CHECK_FALSE(p.StoragesPending());
}

TEST_CASE("A settings game switch opens nothing", "[provider]") {
    // Clicking a profile in the Basic Viewer's Settings panel writes nothing to
    // the provider at all — a profile is an ini section, and only content that
    // needs a game moves the provider onto it. This asserts the weaker property
    // that still has to hold underneath: even the full ApplyIoPathOverrides
    // payload, setter for setter, opens nothing. That is what lets the *active*
    // profile's pages configure freely and let the reopen fall out of the next
    // read. Three quarters of a million entries for StarCraft II and a 149 MB
    // listfile parse for World of Warcraft is what an eager one would cost.
    // StoragesPending() says "configured, not built"; OpenCascCount() is the
    // one that would notice a setter quietly demanding a build.
    FileContentProvider p;
    (void)p.HasCasc(); // a model is loaded, so Warcraft III's storage is up
    REQUIRE_FALSE(p.StoragesPending());
    const std::size_t open = OpenCascCount();

    auto switchTo = [&](ProductId game) {
        p.SetGame(game);
        p.SetListfilePath({});
        p.SetTactKeyPath({});
        p.SetInstallPath(p.GamePath(game));
        p.SetHotsInstallPath(p.HotsPath());
        p.SetIgnoreCasc(false);
        p.SetIgnoreMpq(false);
        p.SetMpqList(FileContentProvider::DefaultMpqList(game));
        CHECK(p.StoragesPending());
        CHECK(OpenCascCount() <= open);
    };
    switchTo(ProductId::Sc2);
    switchTo(ProductId::Wow);

    // Coming back to the game that was open finds it open, and still nothing
    // new was built for the two profiles that were only looked at.
    p.SetGame(ProductId::Wc3);
    CHECK_FALSE(p.StoragesPending());
    CHECK(OpenCascCount() <= open);

    // The other half of the rule the Settings pages follow: an edit to the
    // profile that IS active does have to reopen, because that storage is in
    // use and where it reads from just moved. Invalidation is what makes the
    // next read rebuild it — the edit still opens nothing by itself.
    p.SetInstallPath(p.GamePath(ProductId::Wow)); // any root that is not this one
    CHECK(p.StoragesPending());
    CHECK(OpenCascCount() <= open);
}

TEST_CASE("Switching between two loaded models rebuilds nothing", "[provider]") {
    // Two open documents of different games share one provider, and switching
    // between them must be a pointer move — that is what the per-product slots
    // are for. What made it not one: the viewer re-applied the entering game's
    // ini on every switch, which for World of Warcraft wrote the ini's empty
    // listfile over the one AdoptNearbyWowKeys found beside the model. An ini
    // write that changes a value invalidates the slot, the registry holds
    // installs by weak_ptr, so the provider dropping the last reference
    // DESTROYS the storage — and the next read re-parses indices, manifest and
    // a 144 MB listfile. Measured at ~2.8 s of parse alone, per switch back.
    FileContentProvider p;
    (void)p.HasCasc(); // a Warcraft III model is loaded
    REQUIRE_FALSE(p.StoragesPending());
    const std::size_t open = OpenCascCount();

    p.SetGame(ProductId::Wow); // open a .m2 in a second tab...
    p.SetGame(ProductId::Wc3); // ...and switch back
    CHECK_FALSE(p.StoragesPending());
    CHECK(OpenCascCount() <= open);

    // The mechanism that made it expensive, on the slot that is open: a setter
    // whose value actually moves is what costs a rebuild. Re-applying settings
    // the slot already holds must therefore be something a switch never does.
    p.SetListfilePath(std::filesystem::path("nowhere/listfile.csv"));
    CHECK(p.StoragesPending());
}

TEST_CASE("Settings belong to the game they were made on", "[provider]") {
    FileContentProvider p;
    p.SetGame(ProductId::Wow);
    p.SetIgnoreMpq(true);

    p.SetGame(ProductId::Wc3);
    CHECK_FALSE(p.IgnoreMpq());
    CHECK(p.MpqList() == FileContentProvider::DefaultMpqList());

    p.SetGame(ProductId::Wow);
    CHECK(p.IgnoreMpq());
}

TEST_CASE("Re-applying a setting unchanged opens nothing", "[provider]") {
    FileContentProvider p;
    (void)p.HasCasc();
    REQUIRE_FALSE(p.StoragesPending());

    // What a host does at startup and after every settings edit: write back
    // the values already in place. Treating those as changes is what turned
    // ticking one checkbox into a full reopen of the install.
    p.SetGame(ProductId::Wc3);
    p.SetInstallPath(p.InstallPath());
    p.SetMpqList(p.MpqList());
    p.SetListfilePath(std::filesystem::path(p.ListfilePath()));
    p.SetIgnoreCasc(p.IgnoreCasc());
    p.SetIgnoreMpq(p.IgnoreMpq());
    CHECK_FALSE(p.StoragesPending());

    // A real change still costs what it should.
    p.SetIgnoreCasc(!p.IgnoreCasc());
    CHECK(p.StoragesPending());
}

TEST_CASE("A read served from disk opens no storage", "[provider]") {
    // Every read this process makes before content is loaded is an
    // engine-shipped asset sitting beside the executable — the BLS shader
    // pack, the PSO trace. Opening a game install to answer them is what made
    // startup pay for a CASC parse it then never used.
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "wdx_provider_disk_read";
    std::error_code ec;
    fs::create_directories(dir, ec);
    {
        std::ofstream f(dir / "probe.txt", std::ios::binary);
        f << "probe";
    }

    FileContentProvider p;
    p.SetBasePath(dir);
    const auto bytes = p.ReadFile("probe.txt");
    CHECK(bytes.has_value());
    CHECK(p.StoragesPending());

    fs::remove_all(dir, ec);
}

TEST_CASE("A fresh provider is Warcraft III", "[provider]") {
    // The default has to stay WC3: every host that never calls SetGame is
    // relying on it, and the WC3 render gates would not catch a change here
    // because they configure nothing.
    FileContentProvider p;
    CHECK(p.Game() == ProductId::Wc3);
    CHECK(p.InstallPath() == p.Wc3Path());
    CHECK_FALSE(p.HasListfile());
}

TEST_CASE("Switching product follows that product's install", "[provider]") {
    FileContentProvider p;

    const std::string wow = p.GamePath(ProductId::Wow);
    const std::string sc2 = p.GamePath(ProductId::Sc2);
    const std::string hots = p.HotsPath();
    std::printf("[provider] discovered wc3='%s' wow='%s' sc2='%s' hots='%s'\n",
                p.Wc3Path().c_str(), wow.c_str(), sc2.c_str(), hots.c_str());

    if (wow.empty() && sc2.empty() && hots.empty())
        SKIP("no World of Warcraft or StarCraft II / Heroes install found");

    if (!wow.empty()) {
        p.SetGame(ProductId::Wow);
        CHECK(p.Game() == ProductId::Wow);
        CHECK(p.InstallPath() == wow);
        // Every WoW build since Warlords is CASC. A pre-Warlords install
        // would legitimately have none, so this reports rather than asserts —
        // but the MPQ list must have been rebuilt for the product either way,
        // and must not still be Warcraft III's.
        std::printf("[provider] wow: casc=%d mpq=%d archives=%zu\n", p.HasCasc() ? 1 : 0,
                    p.HasMpq() ? 1 : 0, p.MpqList().size());
        CHECK(p.MpqList() != FileContentProvider::DefaultMpqList(ProductId::Wc3));
    }

    if (!sc2.empty() || !hots.empty()) {
        p.SetGame(ProductId::Sc2);
        CHECK(p.Game() == ProductId::Sc2);
        // CASC-only: no MPQ may be opened for this product whatever is on disk.
        CHECK(p.MpqList().empty());
        CHECK_FALSE(p.HasMpq());
        std::printf("[provider] sc2: casc=%d install='%s'\n", p.HasCasc() ? 1 : 0,
                    p.InstallPath().c_str());
        // The one hard assertion here: an install was found, so a storage has
        // to have opened. If it did not, the product routing is broken in a
        // way no render gate would show — the scene would just be empty.
        CHECK(p.HasCasc());
    }

    // Back to WC3 restores the WC3 configuration, so a host can switch scenes
    // without leaking the previous product's list.
    p.SetGame(ProductId::Wc3);
    CHECK(p.Game() == ProductId::Wc3);
    CHECK(p.InstallPath() == p.Wc3Path());
    CHECK(p.MpqList() == FileContentProvider::DefaultMpqList());
}

TEST_CASE("A WoW storage reads by fileDataID", "[provider]") {
    FileContentProvider p;
    if (p.GamePath(ProductId::Wow).empty())
        SKIP("no World of Warcraft install found");
    p.SetGame(ProductId::Wow);
    if (!p.HasCasc())
        SKIP("World of Warcraft install has no CASC storage (pre-Warlords?)");

    // The distinguishing feature of a WoW root: it names files by id, and an
    // `.m2` references its `.skin` siblings and textures the same way. Without
    // this path a WoW model cannot finish loading at all, so it is worth a
    // direct check rather than inferring it from a render.
    //
    // Scanned rather than hardcoded — a fileDataID that exists is a property
    // of the installed build, and pinning one would rot on the next patch.
    // Bounded so a miss costs a moment, not a minute.
    int hits = 0;
    whiteout::flakes::u32 firstHit = 0;
    for (whiteout::flakes::u32 id = 1; id <= 4096 && hits < 1; ++id) {
        auto bytes = p.ReadFile(whiteout::flakes::ContentRef::FromFileId(id));
        if (bytes && !bytes->empty()) {
            ++hits;
            firstHit = id;
        }
    }
    std::printf("[provider] wow: first fileDataID hit in [1,4096] = %u\n", firstHit);
    CHECK(hits == 1);

    // A miss has to be a clean miss, not a crash or a stale buffer. Ids this
    // large are past the end of any shipped manifest.
    auto absent = p.ReadFile(whiteout::flakes::ContentRef::FromFileId(0xFFFFFFF0u));
    CHECK((!absent || absent->empty()));
}

TEST_CASE("A StarCraft II storage enumerates by path", "[provider]") {
    FileContentProvider p;
    if (p.GamePath(ProductId::Sc2).empty())
        SKIP("no StarCraft II install found");
    p.SetGame(ProductId::Sc2);
    REQUIRE(p.HasCasc());

    // Unlike WoW, an SC2 root carries readable paths, so it browses and
    // path-reads without a listfile. The `.m3` corpus the render gates use is
    // loose files on disk, so this is the only place the CASC route for this
    // product is exercised at all.
    const auto files = p.ListFiles("", true);
    std::printf("[provider] sc2: %zu listed entries\n", files.size());
    CHECK_FALSE(files.empty());
}

TEST_CASE("A StarCraft II storage resolves mod-relative asset paths", "[provider]") {
    FileContentProvider p;
    if (p.GamePath(ProductId::Sc2).empty())
        SKIP("no StarCraft II install found");
    p.SetGame(ProductId::Sc2);
    REQUIRE(p.HasCasc());

    // An `.m3` names its textures relative to whichever mod shipped it
    // ("assets/textures/foo.dds"); the storage stores only mod-rooted full
    // paths ("mods/liberty.sc2mod/base.sc2assets/assets/textures/foo.dds").
    // The provider bridges that with prefixes learned from the listing —
    // asserted here against whatever texture this install actually carries,
    // so the test does not depend on any one shipped file.
    const auto files = p.ListFiles("", true);
    std::string full, rel;
    for (const auto& f : files) {
        const auto at = f.find("/assets/textures/");
        if (at == std::string::npos || f.size() < 5 || f.substr(f.size() - 4) != ".dds")
            continue;
        full = f;
        rel = f.substr(at + 1);
        break;
    }
    if (full.empty())
        SKIP("install lists no mod-rooted assets/textures entry");

    const auto direct = p.ReadFile(full);
    REQUIRE(direct.has_value());
    const auto relative = p.ReadFile(rel);
    // Non-empty is the claim. NOT byte-equality with `full`: several mods can
    // carry the same tail and the fallback deliberately prefers the game's
    // most-derived one, which need not be the mod the listing happened to
    // yield `full` from.
    REQUIRE(relative.has_value());
    CHECK_FALSE(relative->empty());
}

TEST_CASE("The Heroes root is overridable on its own", "[provider]") {
    FileContentProvider p;
    p.SetGame(ProductId::Sc2);

    // StarCraft II's install path cannot name Heroes' install too, which is
    // the whole reason this second root exists. Pure policy — it holds whether
    // or not either game is installed, so no skip.
    const std::string discovered = p.HotsPath();
    CHECK(p.HotsInstallPath() == discovered);

    p.SetHotsInstallPath("D:/nowhere/heroes");
    CHECK(p.HotsInstallPath() == "D:/nowhere/heroes");
    // Overriding one root must not move the other.
    CHECK(p.InstallPath() == p.GamePath(ProductId::Sc2));

    p.SetHotsInstallPath("");
    CHECK(p.HotsInstallPath() == discovered);

    // A bogus root opens nothing, and OpenCascRoots reports exactly the
    // storages that did open — the fact the Settings page needs when two are
    // offered and one may fail.
    p.SetHotsInstallPath("D:/nowhere/heroes");
    for (const auto& r : p.OpenCascRoots())
        CHECK(r != "D:/nowhere/heroes");
    CHECK(p.OpenCascRoots().empty() == !p.HasCasc());
}

TEST_CASE("StarCraft II and Heroes can be open at once", "[provider]") {
    FileContentProvider p;
    if (p.GamePath(ProductId::Sc2).empty() || p.HotsPath().empty())
        SKIP("needs BOTH StarCraft II and Heroes of the Storm installed");

    // The reason the provider holds a vector of storages rather than one: the
    // two games share ProductId::Sc2 because they share a render profile, so a
    // scene set to Sc2 has to be able to read either.
    p.SetGame(ProductId::Sc2);
    CHECK(p.HasCasc());
    const auto files = p.ListFiles("", true);
    std::printf("[provider] sc2+hots: %zu listed entries\n", files.size());
    CHECK_FALSE(files.empty());
}

TEST_CASE("One install opens once, however many readers it has", "[provider][casc]") {
    FileContentProvider probe;
    if (probe.Wc3Path().empty())
        SKIP("no Warcraft III install found");

    // Every scene has its own provider and the Storage Explorer has neither —
    // and opening a CASC parses its indices, encoding table and root manifest,
    // which is seconds and hundreds of megabytes. Doing that per reader is what
    // the registry exists to stop, so what is checked is the count of open
    // installs, not that reads happen to work.
    const std::size_t before = OpenCascCount();
    {
        FileContentProvider a;
        FileContentProvider b;
        REQUIRE(a.HasCasc());
        REQUIRE(b.HasCasc());
        CHECK(a.OpenCascRoots() == b.OpenCascRoots());

        StorageBrowser browser;
        std::string error;
        REQUIRE(browser.Open(probe.Wc3Path(), StorageKind::Casc, &error));

        // Three readers, one storage. The +1 is Warcraft III's, whether or not
        // another test in this binary already had it open.
        CHECK(OpenCascCount() <= before + 1);
        CHECK(OpenCascCount() >= 1);

        // ...and both readers genuinely read it. Named from the storage rather
        // than hardcoded: which models a Warcraft III install ships is a
        // property of the build, not something a test should assert.
        std::string model;
        for (const auto& f : a.ListFiles("units/human", true)) {
            if (f.size() > 4 && f.compare(f.size() - 4, 4, ".mdx") == 0) {
                model = f;
                break;
            }
        }
        REQUIRE_FALSE(model.empty());
        auto viaA = a.ReadFile(model);
        auto viaB = b.ReadFile(model);
        REQUIRE(viaA);
        REQUIRE(viaB);
        CHECK(*viaA == *viaB);
    }
    // Weakly held, so the last reader closing it frees it rather than leaving
    // an install open for the life of the process.
    CHECK(OpenCascCount() <= before);
}
