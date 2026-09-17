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
#include "io/storage/storage_paths.h"
#include "io/storage_browser.h"
#include "whiteout/flakes/content_ref.h"

#include <whiteout/models/mdx/parser.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <fstream>
#include <string>

using whiteout::flakes::ProductId;
using whiteout::flakes::Wc3ArtTier;
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
    p.SetGame(ProductId::D3);
    p.SetGame(ProductId::Wc3);
    p.SetIgnoreMpq(true);
    p.SetMpqList({});
    CHECK(p.StoragesPending());

    // Asking a question only an open storage can answer is a demand, and pays
    // for it once.
    (void)p.HasCasc();
    CHECK_FALSE(p.StoragesPending());
}

TEST_CASE("Every ProductId has a slot", "[provider]") {
    // The provider's per-game slots live in a fixed-size array indexed directly
    // by ProductId, and each holds a StorageConfig plus a unique_ptr. Adding a
    // product without growing that array is a heap overrun on the first
    // SetGame, not a benign read — which is exactly the kind of thing a build
    // stays quiet about. Round-tripping every value is what catches it.
    FileContentProvider p;
    for (ProductId g : {ProductId::Wc3, ProductId::Wow, ProductId::Sc2, ProductId::D3}) {
        p.SetGame(g);
        CHECK(p.Game() == g);
        // Reads the slot's own configured path, which is where an overrun
        // would land.
        (void)p.GamePath(g);
    }
    CHECK(p.StoragesPending()); // still nothing opened by configuration alone
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
    switchTo(ProductId::D3);
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

TEST_CASE("A Warcraft III storage reads every art tier", "[provider][tier]") {
    FileContentProvider p;
    if (p.GamePath(ProductId::Wc3).empty())
        SKIP("no Warcraft III install found");
    p.SetGame(ProductId::Wc3);
    if (!p.HasCasc())
        SKIP("Warcraft III install has no CASC storage (pre-Reforged?)");

    // Every tier's own spelling of one path that all three ship. The chained
    // spelling reads verbatim, whatever tier the provider is set to: it names
    // its overlay outright, and a prefix in front of it would only spell
    // "war3.w3mod:war3.w3mod:...".
    const char* kTiered[] = {
        "war3.w3mod:units\\human\\footman\\footman.mdx",
        "war3.w3mod:_hd.w3mod:units\\human\\footman\\footman.mdx",
        "war3.w3mod:_de.w3mod:units\\human\\footman\\footman.mdx",
    };
    std::size_t sizes[3] = {0, 0, 0};
    for (int i = 0; i < 3; ++i) {
        auto bytes = p.ReadFile(kTiered[i]);
        if (bytes)
            sizes[i] = bytes->size();
        std::printf("[provider] wc3 tier %d footman: %zu bytes\n", i, sizes[i]);
    }
    // A 3.0.0 install has all three. An older one has no `_de.w3mod` at all,
    // and saying so is more useful than failing: the tier chain is still right,
    // there is just nothing in that overlay to find.
    CHECK(sizes[0] > 0);
    CHECK(sizes[1] > 0);
    if (sizes[2] == 0) {
        WARN("no _de.w3mod overlay — install predates Warcraft III 3.0.0");
        return;
    }
    // Three different files, not three names for one. If a chained read were
    // being resolved through the chain instead of verbatim, two of these would
    // come back identical.
    CHECK(sizes[0] != sizes[1]);
    CHECK(sizes[1] != sizes[2]);

    // The unchained spelling — what a model's own texture and child-model
    // references look like — resolves through whichever tier is selected, and
    // that is the whole point of the setting.
    const char* kBare = "units\\human\\footman\\footman.mdx";
    p.SetArtTier(Wc3ArtTier::Classic);
    auto classic = p.ReadFile(kBare);
    p.SetArtTier(Wc3ArtTier::Reforged);
    auto reforged = p.ReadFile(kBare);
    p.SetArtTier(Wc3ArtTier::Definitive);
    auto definitive = p.ReadFile(kBare);
    REQUIRE(classic);
    REQUIRE(reforged);
    REQUIRE(definitive);
    CHECK(classic->size() == sizes[0]);
    CHECK(reforged->size() == sizes[1]);
    CHECK(definitive->size() == sizes[2]);

    // And the part that was actually broken: a file only the Definitive
    // overlay has. Before 3.0.0 support it was unreachable under every tier,
    // because no chain named `_de.w3mod:` — so the model loaded and then
    // hunted its textures through overlays that do not hold them.
    //
    // Scanned rather than hardcoded: which paths are Definitive-only is a
    // property of the installed build.
    StorageBrowser b;
    b.SetOpenTypes(whiteout::flakes::io::BrowseType::Models);
    std::string err;
    if (!b.Open(p.InstallPath(), StorageKind::Casc, &err))
        SKIP("could not browse the Warcraft III install: " + err);

    // Walk the Definitive overlay for a model the older ones do not have. The
    // browser is the right way in rather than a second enumeration: it is what
    // the CASC Browser itself lists from, so this also checks that the display
    // path it hands back is one the provider can actually read.
    std::string deOnly;
    const std::function<void(const std::string&, int)> walk =
        [&](const std::string& dir, int depth) {
            if (!deOnly.empty() || depth > 8)
                return;
            const auto kids = b.TreeChildren(dir);
            for (const std::string& f : kids.files) {
                if (f.size() < 4 || f.compare(f.size() - 4, 4, ".mdx") != 0)
                    continue;
                const std::string archive = b.ChildPathAt(dir, f);
                const auto tier = whiteout::flakes::io::Wc3TierOfPath(archive);
                if (tier != Wc3ArtTier::Definitive)
                    continue;
                // The path as a model would reference it: no chain at all.
                const std::string bare =
                    std::string(whiteout::flakes::io::StripWc3ModRoot(archive))
                        .substr(std::strlen("_de.w3mod:"));
                // Asked of each older overlay by name — a tier read would now
                // reach `_de` last and find it anyway.
                const auto inHd = p.ReadFile("war3.w3mod:_hd.w3mod:" + bare);
                const auto inSd = p.ReadFile("war3.w3mod:" + bare);
                if ((inHd && !inHd->empty()) || (inSd && !inSd->empty()))
                    continue; // the older overlays have it too; not a witness
                deOnly = bare;
                return;
            }
            for (const std::string& sub : kids.folders) {
                // Skip a nested sub-mod (`_teen.w3mod`, `_tilesets\a.w3mod`) —
                // the browser shows each as a folder, so it has no ':' left to
                // spot it by. Its files resolve fine, but a witness carrying a
                // second chain proves less about an ordinary model reference
                // than a plain `abilities\...` path does.
                if (sub.size() >= 6 && sub.compare(sub.size() - 6, 6, ".w3mod") == 0)
                    continue;
                walk(dir.empty() ? sub : dir + "\\" + sub, depth + 1);
                if (!deOnly.empty())
                    return;
            }
        };
    walk("_de.w3mod", 0);

    if (deOnly.empty()) {
        WARN("no Definitive-only .mdx found to check");
        return;
    }
    std::printf("[provider] wc3 Definitive-only model: %s\n", deOnly.c_str());
    p.SetArtTier(Wc3ArtTier::Definitive);
    auto found = p.ReadFile(deOnly);
    REQUIRE((found && !found->empty()));
    // The older tiers reach `_de` last, so a file only it has still resolves
    // under them — and it is the same file, not a stand-in.
    for (Wc3ArtTier older : {Wc3ArtTier::Reforged, Wc3ArtTier::Classic}) {
        p.SetArtTier(older);
        auto fallback = p.ReadFile(deOnly);
        REQUIRE(fallback);
        CHECK(fallback->size() == found->size());
    }
}

TEST_CASE("A cinematic under the root reads its _de-only textures", "[provider][tier]") {
    FileContentProvider p;
    if (p.GamePath(ProductId::Wc3).empty())
        SKIP("no Warcraft III install found");
    p.SetGame(ProductId::Wc3);
    if (!p.HasCasc())
        SKIP("Warcraft III install has no CASC storage (pre-Reforged?)");

    // 3.0.0's cinematics sit under `war3.w3mod:cinematics\`, outside every
    // overlay, and are HD — so they read the Reforged tier — yet much of their
    // art exists only under `_de.w3mod`. Scanned, not hardcoded: which
    // cinematic has such a texture is a property of the installed build.
    StorageBrowser b;
    b.SetOpenTypes(whiteout::flakes::io::BrowseType::Models);
    std::string err;
    if (!b.Open(p.InstallPath(), StorageKind::Casc, &err))
        SKIP("could not browse the Warcraft III install: " + err);

    constexpr int kMaxModels = 12; // each parse is several MB
    int parsed = 0;
    for (const std::string& folder : b.TreeChildren("cinematics").folders) {
        const std::string dir = "cinematics\\" + folder;
        for (const std::string& f : b.TreeChildren(dir).files) {
            if (f.size() < 4 || f.compare(f.size() - 4, 4, ".mdx") != 0)
                continue;
            if (parsed++ >= kMaxModels)
                break;
            const std::string archive = b.ChildPathAt(dir, f);
            const auto bytes = p.ReadFile(archive);
            if (!bytes)
                continue;
            whiteout::mdx::Model model;
            try {
                whiteout::mdx::Parser parser;
                model = parser.parse(std::span<const whiteout::u8>(*bytes));
            } catch (const std::exception&) {
                continue;
            }
            int deOnly = 0, resolved = 0;
            for (const auto& tex : model.textures) {
                if (tex.fileName.empty())
                    continue;
                const auto inDe = p.ReadFile("war3.w3mod:_de.w3mod:" + tex.fileName);
                const auto inHd = p.ReadFile("war3.w3mod:_hd.w3mod:" + tex.fileName);
                const auto inSd = p.ReadFile("war3.w3mod:" + tex.fileName);
                if (!inDe || inDe->empty() || (inHd && !inHd->empty()) || (inSd && !inSd->empty()))
                    continue;
                ++deOnly;
                p.SetArtTier(Wc3ArtTier::Reforged);
                const auto read = p.ReadFile(tex.fileName);
                if (read && read->size() == inDe->size())
                    ++resolved;
            }
            if (deOnly == 0)
                continue;
            std::printf("[provider] wc3 %s: %d _de-only textures, %d resolved under Reforged\n",
                        archive.c_str(), deOnly, resolved);
            CHECK(resolved == deOnly);
            return;
        }
        if (parsed >= kMaxModels)
            break;
    }
    WARN("no cinematic with a _de-only texture found to check");
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
