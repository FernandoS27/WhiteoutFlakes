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
        path = whiteout::flakes::ExecutableDir() / name;
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
