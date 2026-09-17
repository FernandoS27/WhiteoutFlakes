// ============================================================================
// Per-game IO overrides in the viewer's settings ini.
//
// The Settings window grew a game picker, so the single `[IO]` section became
// one section per product. Two things can break silently and neither shows up
// in a render gate: a save for one game overwriting another's, and Warcraft
// III's section moving (which would drop every existing user's install-path
// override on upgrade).
//
// Device-free — this is file parsing — so it is a G0 test. Every case
// redirects the ini into the build tree first: the real location is a shared
// per-user config directory on Linux and macOS, and a test has no business
// rewriting the developer's settings.
// ============================================================================

#include <catch2/catch_test_macros.hpp>

#include "settings_ini.h"
#include "storage_explorer_ini.h"
#include "whiteout/flakes/util/path_utf8.h"

#include <fstream>
#include <string>

using whiteout::flakes::IoPathOverrides;
using whiteout::flakes::LoadIoPathOverrides;
using whiteout::flakes::LoadIoProduct;
using whiteout::flakes::ProductId;
using whiteout::flakes::SaveIoPathOverrides;
using whiteout::flakes::SaveIoProduct;

namespace {

// Fresh file per case, so one case's writes cannot satisfy another's reads.
struct ScopedIni {
    explicit ScopedIni(const char* name) {
        path = whiteout::flakes::io::ExecutableDirectory() / name;
        std::error_code ec;
        std::filesystem::remove(path, ec);
        whiteout::flakes::SetSettingsIniPathOverride(path);
    }
    ~ScopedIni() {
        whiteout::flakes::SetSettingsIniPathOverride({});
    }
    std::filesystem::path path;
};

// Distinct, recognisable state per product so a cross-write is visible rather
// than merely wrong.
IoPathOverrides Sample(const std::string& tag) {
    IoPathOverrides o;
    o.installPath = "D:/games/" + tag;
    o.ignoreCasc = (tag == "wow");
    o.ignoreMpq = (tag == "sc2");
    o.mpqListSet = true;
    o.mpqList = {tag + "-a.mpq", tag + "-b.mpq"};
    o.listfilePath = "D:/lists/" + tag + ".csv";
    o.hotsInstallPath = "D:/games/" + tag + "-hots";
    return o;
}

} // namespace

TEST_CASE("Each game's IO overrides round-trip independently", "[settings]") {
    ScopedIni ini("test_io_pergame.ini");
    SaveIoPathOverrides(ProductId::Wc3, Sample("wc3"));
    SaveIoPathOverrides(ProductId::Wow, Sample("wow"));
    SaveIoPathOverrides(ProductId::Sc2, Sample("sc2"));

    // Written last-to-first would still pass if they shared a section, so read
    // all three back after all three writes.
    const auto wc3 = LoadIoPathOverrides(ProductId::Wc3);
    const auto wow = LoadIoPathOverrides(ProductId::Wow);
    const auto sc2 = LoadIoPathOverrides(ProductId::Sc2);

    CHECK(wc3.installPath == "D:/games/wc3");
    CHECK(wow.installPath == "D:/games/wow");
    CHECK(sc2.installPath == "D:/games/sc2");

    CHECK_FALSE(wc3.ignoreCasc);
    CHECK(wow.ignoreCasc);
    CHECK(sc2.ignoreMpq);

    REQUIRE(wc3.mpqList.size() == 2);
    CHECK(wc3.mpqList[0] == "wc3-a.mpq");
    REQUIRE(wow.mpqList.size() == 2);
    CHECK(wow.mpqList[1] == "wow-b.mpq");

    // Single-product settings are written only for the product that reads
    // them — a listfile key under [IO.Sc2] would be a setting nothing consults.
    CHECK(wow.listfilePath == "D:/lists/wow.csv");
    CHECK(wc3.listfilePath.empty());
    CHECK(sc2.listfilePath.empty());
    CHECK(sc2.hotsInstallPath == "D:/games/sc2-hots");
    CHECK(wc3.hotsInstallPath.empty());
    CHECK(wow.hotsInstallPath.empty());

    // The no-argument overloads still mean Warcraft III: every pre-existing
    // caller (the Max plugin, the viewer's startup path) is written that way.
    CHECK(LoadIoPathOverrides().installPath == wc3.installPath);
}

TEST_CASE("Warcraft III keeps the bare [IO] section", "[settings]") {
    ScopedIni ini("test_io_legacy.ini");
    // Not a style point: an ini written before the settings grew a game picker
    // has `[IO] InstallPath=...`, and moving WC3 to `[IO.Wc3]` would silently
    // discard it on the next launch.
    IoPathOverrides o;
    o.installPath = "D:/games/legacy";
    SaveIoPathOverrides(o);

    std::ifstream f(ini.path);
    REQUIRE(f);
    std::string line, section;
    bool found = false;
    while (std::getline(f, line)) {
        if (!line.empty() && line.front() == '[')
            section = line;
        if (line.rfind("InstallPath=D:/games/legacy", 0) == 0) {
            CHECK(section == "[IO]");
            found = true;
        }
    }
    CHECK(found);
}

TEST_CASE("The selected game persists", "[settings]") {
    ScopedIni ini("test_io_product.ini");
    // Unset is Warcraft III — a fresh install must not come up on a game the
    // user never picked.
    CHECK(LoadIoProduct() == ProductId::Wc3);
    SaveIoProduct(ProductId::Sc2);
    CHECK(LoadIoProduct() == ProductId::Sc2);
    SaveIoProduct(ProductId::Wow);
    CHECK(LoadIoProduct() == ProductId::Wow);
}

// ============================================================================
// Where the Storage Explorer was left.
//
// One section, eight keys, and two things that would break quietly: writing it
// dropping the IO sections beside it (they share one file), and a key the
// panel reads as "not recorded" being written as a value that means something
// else. `BrowseTypes=0` is the second kind - restored literally it is a panel
// browsing for no file type at all, which looks exactly like an empty storage.
// ============================================================================

namespace {

using whiteout::flakes::io::BrowseType;
using whiteout::flakes::tools::ExplorerState;
using whiteout::flakes::tools::ExplorerView;

ExplorerState SampleState() {
    ExplorerState st;
    st.view = ExplorerView::Tree;
    st.game = ProductId::Sc2;
    st.browseTypes = BrowseType::M3;
    // A real display path: backslashes and spaces, both of which an ini value
    // has to carry verbatim.
    st.folder = "campaigns\\liberty.sc2campaign\\base.sc2assets\\assets\\doodads";
    st.filter = "marine, -death";
    st.selected = "assets\\units\\terran\\marine\\marine.m3";
    st.iconSize = 96.0f;
    st.treeSplit = 412.0f;
    return st;
}

} // namespace

TEST_CASE("The Storage Explorer comes back where it was left", "[settings]") {
    ScopedIni ini("test_explorer.ini");

    // Nothing written yet: the defaults, not an empty panel.
    const ExplorerState fresh = whiteout::flakes::LoadStorageExplorerState();
    CHECK(fresh.view == ExplorerView::Grid);
    CHECK(fresh.game == ProductId::Neutral);
    CHECK(fresh.browseTypes == BrowseType::None);
    CHECK(fresh.folder.empty());
    CHECK(fresh.iconSize > 0.0f);
    CHECK(fresh.treeSplit > 0.0f);

    const ExplorerState saved = SampleState();
    whiteout::flakes::SaveStorageExplorerState(saved);
    const ExplorerState back = whiteout::flakes::LoadStorageExplorerState();

    CHECK(back.view == saved.view);
    CHECK(back.game == saved.game);
    CHECK(back.heroes == saved.heroes);
    CHECK(back.browseTypes == saved.browseTypes);
    CHECK(back.folder == saved.folder);
    CHECK(back.filter == saved.filter);
    CHECK(back.selected == saved.selected);
    CHECK(back.iconSize == saved.iconSize);
    CHECK(back.treeSplit == saved.treeSplit);

    // The key is what a host polls instead of rewriting the file. It has to
    // move when the state does and hold still when it doesn't - including
    // across the save/load, or every restored session would write once for
    // nothing.
    CHECK(whiteout::flakes::ExplorerStateKey(back) ==
          whiteout::flakes::ExplorerStateKey(saved));
    ExplorerState moved = saved;
    moved.folder += "\\props";
    CHECK(whiteout::flakes::ExplorerStateKey(moved) !=
          whiteout::flakes::ExplorerStateKey(saved));
    // Sub-pixel drags must not: the file stores whole pixels, so a key that
    // moved for one would rewrite the whole settings file for a mouse tremor.
    ExplorerState nudged = saved;
    nudged.treeSplit += 0.4f;
    CHECK(whiteout::flakes::ExplorerStateKey(nudged) ==
          whiteout::flakes::ExplorerStateKey(saved));
}

TEST_CASE("Heroes of the Storm survives the round trip as its own target", "[settings]") {
    ScopedIni ini("test_explorer_hots.ini");

    // Heroes is not a ProductId - it shares Sc2 with StarCraft II because it
    // shares a render profile - so "which game was the panel on" is the pair,
    // and a file that recorded only the product brings a Heroes session back
    // on StarCraft II.
    ExplorerState st = SampleState();
    st.heroes = true;
    st.folder = R"(mods\heroes.stormmod\base.stormassets\assets\units\heroes)";
    whiteout::flakes::SaveStorageExplorerState(st);

    const ExplorerState back = whiteout::flakes::LoadStorageExplorerState();
    CHECK(back.game == ProductId::Sc2);
    CHECK(back.heroes);
    CHECK(back.folder == st.folder);

    // The two targets are different states, so a host polling the key writes
    // when the user switches between them.
    ExplorerState sc2 = st;
    sc2.heroes = false;
    CHECK(whiteout::flakes::ExplorerStateKey(sc2) != whiteout::flakes::ExplorerStateKey(st));

    // And StarCraft II still round-trips as itself.
    whiteout::flakes::SaveStorageExplorerState(sc2);
    const ExplorerState backSc2 = whiteout::flakes::LoadStorageExplorerState();
    CHECK(backSc2.game == ProductId::Sc2);
    CHECK_FALSE(backSc2.heroes);
}

TEST_CASE("Saving the explorer section keeps the IO sections beside it", "[settings]") {
    ScopedIni ini("test_explorer_coexist.ini");

    IoPathOverrides wow;
    wow.installPath = "D:\\Games\\World of Warcraft";
    wow.listfilePath = "C:\\lists\\community.csv";
    SaveIoPathOverrides(ProductId::Wow, wow);
    SaveIoProduct(ProductId::Wow);

    whiteout::flakes::SaveStorageExplorerState(SampleState());

    // Both halves of the file survive the other's write.
    const IoPathOverrides backWow = LoadIoPathOverrides(ProductId::Wow);
    CHECK(backWow.installPath == wow.installPath);
    CHECK(backWow.listfilePath == wow.listfilePath);
    CHECK(LoadIoProduct() == ProductId::Wow);
    CHECK(whiteout::flakes::LoadStorageExplorerState().game == ProductId::Sc2);

    // And the reverse order: the IO write must not drop the explorer section.
    SaveIoPathOverrides(ProductId::Wow, wow);
    CHECK(whiteout::flakes::LoadStorageExplorerState().folder == SampleState().folder);
}

TEST_CASE("An unrecorded browse-type mask restores the game's own default", "[settings]") {
    ScopedIni ini("test_explorer_types.ini");

    // What a panel that never had its checkboxes touched writes, and what a
    // hand-trimmed ini leaves behind. Restored literally it would be a panel
    // browsing for nothing, which reads as an empty storage.
    ExplorerState st = SampleState();
    st.browseTypes = BrowseType::None;
    whiteout::flakes::SaveStorageExplorerState(st);
    CHECK(whiteout::flakes::LoadStorageExplorerState().browseTypes == BrowseType::None);

    // The rest of the section still round-trips around it.
    CHECK(whiteout::flakes::LoadStorageExplorerState().folder == st.folder);
}
