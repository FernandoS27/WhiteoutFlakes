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
#include "whiteout/flakes/content_ref.h"

#include <cstdio>
#include <string>

using whiteout::flakes::ProductId;
using whiteout::flakes::io::FileContentProvider;

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
