// ============================================================================
// Where settings live, and the half of them that configure content storage.
//
// Split out of settings_ini.cpp so that reading the IO section does not link
// the renderer: the display settings talk to RenderService, which pulls in the
// gfx backends (and, on a WebGPU build, its runtime DLL). Nothing here needs
// more than a FileContentProvider, which is what lets the unit test exercise
// the per-game ini round-trip in a device-free build.
// ============================================================================

#include "settings_ini.h"

#include "ini_file.h"
#include "io/file_content_provider.h"
#include "whiteout/flakes/types.h"
#include "whiteout/flakes/util/path_utf8.h"

#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

namespace whiteout::flakes {

using ini::IniMap;
using ini::ParseBool;

namespace {

namespace fs = std::filesystem;

// Set by SetSettingsIniPathOverride; empty means "use the per-OS location".
fs::path g_iniPathOverride;

// Per-OS config location:
//   • Windows: alongside the exe (preserves prior behaviour).
//   • Linux:   $XDG_CONFIG_HOME/WhiteoutFlakes/ (or ~/.config/...).
//   • macOS:   ~/Library/Application Support/WhiteoutFlakes/ (Apple convention,
//              and the .app bundle is read-only anyway).
fs::path DefaultSettingsIniPath() {
#ifdef _WIN32
    return io::ExecutableDirectory() / "WhiteoutFlakes.ini";
#elif defined(__APPLE__)
    fs::path base;
    if (const char* home = std::getenv("HOME"); home && *home)
        base = fs::path(home) / "Library" / "Application Support";
    else
        base = ".";
    fs::path dir = base / "WhiteoutFlakes";
    std::error_code ec;
    fs::create_directories(dir, ec);
    return dir / "settings.ini";
#else
    fs::path base;
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg) {
        base = xdg;
    } else if (const char* home = std::getenv("HOME"); home && *home) {
        base = fs::path(home) / ".config";
    } else {
        base = ".";
    }
    fs::path dir = base / "WhiteoutFlakes";
    std::error_code ec;
    fs::create_directories(dir, ec);
    return dir / "settings.ini";
#endif
}

constexpr const char* kIoSection = "IO";

// Warcraft III keeps the bare `[IO]` section it has always written, so an ini
// from before the settings grew a per-game profile still applies. The other
// products suffix it.
std::string IoSectionOf(ProductId game) {
    switch (game) {
    case ProductId::Wow:
        return std::string(kIoSection) + ".Wow";
    case ProductId::Sc2:
        return std::string(kIoSection) + ".Sc2";
    case ProductId::D3:
        return std::string(kIoSection) + ".D3";
    default:
        return kIoSection;
    }
}

std::string IoKeyOf(ProductId game, const char* k) {
    return IoSectionOf(game) + "." + k;
}

std::string IoKeyOf(const char* k) {
    return IoKeyOf(ProductId::Wc3, k);
}

// The MPQ list serialises as pipe-separated filenames inside one ini value;
// '|' is illegal in NTFS filenames and ASCII-printable so it never collides
// with a real entry and stays human-readable in the .ini.
std::string JoinMpqList(const std::vector<std::string>& list) {
    std::string out;
    for (usize i = 0; i < list.size(); ++i) {
        if (i)
            out += '|';
        out += list[i];
    }
    return out;
}

std::vector<std::string> SplitMpqList(const std::string& s) {
    std::vector<std::string> out;
    if (s.empty())
        return out;
    usize start = 0;
    while (start <= s.size()) {
        const usize bar = s.find('|', start);
        const usize end = (bar == std::string::npos) ? s.size() : bar;
        if (end > start)
            out.emplace_back(s.substr(start, end - start));
        if (bar == std::string::npos)
            break;
        start = bar + 1;
    }
    return out;
}

} // namespace

fs::path SettingsIniPath() {
    return g_iniPathOverride.empty() ? DefaultSettingsIniPath() : g_iniPathOverride;
}

void SetSettingsIniPathOverride(const fs::path& file) {
    g_iniPathOverride = file;
}

fs::path AssetDir() {
    // io::ExecutableDirectory already answers Contents/Resources inside a macOS
    // bundle, which is where the bundled read-only data ships.
    fs::path dir = io::ExecutableDirectory();
    return dir.empty() ? fs::current_path() : dir;
}

IoPathOverrides LoadIoPathOverrides() {
    return LoadIoPathOverrides(ProductId::Wc3);
}

IoPathOverrides LoadIoPathOverrides(ProductId game) {
    IniMap ini;
    ini.Load(SettingsIniPath());
    IoPathOverrides o;
    if (auto* s = ini.Get(IoKeyOf(game, "InstallPath")))
        o.installPath = *s;
    if (auto* s = ini.Get(IoKeyOf(game, "IgnoreCasc"))) {
        bool v = false;
        if (ParseBool(*s, v))
            o.ignoreCasc = v;
    }
    if (auto* s = ini.Get(IoKeyOf(game, "IgnoreMpq"))) {
        bool v = false;
        if (ParseBool(*s, v))
            o.ignoreMpq = v;
    }
    if (auto* s = ini.Get(IoKeyOf(game, "MpqList"))) {
        o.mpqListSet = true;
        o.mpqList = SplitMpqList(*s);
    }
    if (auto* s = ini.Get(IoKeyOf(game, "Listfile")))
        o.listfilePath = *s;
    if (auto* s = ini.Get(IoKeyOf(game, "TactKeys")))
        o.tactKeyPath = *s;
    if (auto* s = ini.Get(IoKeyOf(game, "HotsInstallPath")))
        o.hotsInstallPath = *s;
    return o;
}

void SaveIoPathOverrides(const IoPathOverrides& overrides) {
    SaveIoPathOverrides(ProductId::Wc3, overrides);
}

void SaveIoPathOverrides(ProductId game, const IoPathOverrides& overrides) {
    const fs::path path = SettingsIniPath();
    // Round-trip existing keys so writing one game's IO section doesn't drop
    // Display, or the other games'.
    IniMap ini;
    ini.Load(path);
    ini.Set(IoKeyOf(game, "InstallPath"), overrides.installPath);
    ini.Set(IoKeyOf(game, "IgnoreCasc"), overrides.ignoreCasc ? "1" : "0");
    ini.Set(IoKeyOf(game, "IgnoreMpq"), overrides.ignoreMpq ? "1" : "0");
    // Only persist MpqList when the user has explicitly customised it —
    // omitting the key on first save means future provider versions that
    // change DefaultMpqList() are picked up automatically for these users.
    if (overrides.mpqListSet)
        ini.Set(IoKeyOf(game, "MpqList"), JoinMpqList(overrides.mpqList));
    // Both are single-product settings; writing them for the others would put
    // a key in a section that never reads it.
    if (game == ProductId::Wow) {
        ini.Set(IoKeyOf(game, "Listfile"), overrides.listfilePath);
        ini.Set(IoKeyOf(game, "TactKeys"), overrides.tactKeyPath);
    }
    if (game == ProductId::Sc2)
        ini.Set(IoKeyOf(game, "HotsInstallPath"), overrides.hotsInstallPath);
    ini.Save(path);
}

// Stable strings rather than the enum value: ProductId is an ABI enum whose
// numbering is not a promise the ini should depend on.
ProductId LoadIoProduct() {
    IniMap ini;
    ini.Load(SettingsIniPath());
    if (auto* s = ini.Get(IoKeyOf("Product"))) {
        if (*s == "wow")
            return ProductId::Wow;
        if (*s == "sc2")
            return ProductId::Sc2;
        if (*s == "d3")
            return ProductId::D3;
    }
    return ProductId::Wc3;
}

void SaveIoProduct(ProductId game) {
    const fs::path path = SettingsIniPath();
    IniMap ini;
    ini.Load(path);
    const char* name = "wc3";
    if (game == ProductId::Wow)
        name = "wow";
    else if (game == ProductId::Sc2)
        name = "sc2";
    else if (game == ProductId::D3)
        name = "d3";
    ini.Set(IoKeyOf("Product"), name);
    ini.Save(path);
}

void ApplyIoPathOverrides(io::FileContentProvider& provider, ProductId game) {
    const IoPathOverrides o = LoadIoPathOverrides(game);
    // Game first: every setting below belongs to one product's slot, so
    // applying any of them before the switch would write them into whichever
    // game the provider happened to be on. Nothing here reopens anything by
    // itself — a setter that changes nothing is a no-op, and one that does
    // only marks its own game's storages for the next read to rebuild.
    provider.SetGame(game);
    provider.SetListfilePath(io::FsPathFromUtf8(o.listfilePath));
    provider.SetTactKeyPath(io::FsPathFromUtf8(o.tactKeyPath));
    if (!o.installPath.empty())
        provider.SetInstallPath(o.installPath);
    if (!o.hotsInstallPath.empty())
        provider.SetHotsInstallPath(o.hotsInstallPath);
    provider.SetIgnoreCasc(o.ignoreCasc);
    provider.SetIgnoreMpq(o.ignoreMpq);
    if (o.mpqListSet)
        provider.SetMpqList(o.mpqList);
}

} // namespace whiteout::flakes
