#include "settings_ini.h"

#include "renderer/assets/replaceable_texture_manager.h"
#include "renderer/render_service.h"
#include "whiteout/flakes/types.h"

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__linux__)
#include <unistd.h>
#include <climits>
#endif

namespace whiteout::flakes {

using namespace whiteout::flakes::renderer;
using namespace whiteout::flakes::renderer::assets;

namespace {

namespace fs = std::filesystem;

// Per-OS config location:
//   • Windows: alongside the exe (preserves prior behaviour).
//   • Linux:   $XDG_CONFIG_HOME/WhiteoutFlakes/ (or ~/.config/...).
//   • macOS:   ~/Library/Application Support/WhiteoutFlakes/ (Apple convention,
//              and the .app bundle is read-only anyway).
fs::path SettingsIniPath() {
#ifdef _WIN32
    wchar_t exePath[MAX_PATH] = {};
    DWORD n = ::GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    if (n == 0 || n >= MAX_PATH)
        return fs::path("WhiteoutFlakes.ini");
    fs::path p(exePath);
    p.replace_filename(L"WhiteoutFlakes.ini");
    return p;
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

// Tiny `[Section]\nkey=value` reader/writer. The viewer fully owns the file
// (no third-party keys to preserve) so we just round-trip everything as a
// single Display section.
struct IniMap {
    std::unordered_map<std::string, std::string> values;

    static std::string Trim(std::string_view s) {
        const auto isWs = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
        usize a = 0, b = s.size();
        while (a < b && isWs(s[a]))
            ++a;
        while (b > a && isWs(s[b - 1]))
            --b;
        return std::string(s.substr(a, b - a));
    }

    void Load(const fs::path& path) {
        std::ifstream f(path);
        if (!f)
            return;
        std::string line;
        std::string section;
        while (std::getline(f, line)) {
            std::string t = Trim(line);
            if (t.empty() || t[0] == ';' || t[0] == '#')
                continue;
            if (t.front() == '[' && t.back() == ']') {
                section = t.substr(1, t.size() - 2);
                continue;
            }
            const auto eq = t.find('=');
            if (eq == std::string::npos)
                continue;
            std::string key = Trim(std::string_view(t).substr(0, eq));
            std::string val = Trim(std::string_view(t).substr(eq + 1));
            if (section.empty())
                values[key] = std::move(val);
            else
                values[section + "." + key] = std::move(val);
        }
    }

    // Writes every key as `[section]\nkey=value\n`, grouping by the prefix
    // before the first '.'. Keys without a '.' are skipped (they'd be
    // section-less and we don't emit those today).
    void Save(const fs::path& path) const {
        std::ofstream f(path, std::ios::trunc);
        if (!f)
            return;
        // Ordered so the file diff stays stable across saves regardless of
        // unordered_map's iteration order.
        std::map<std::string, std::vector<std::pair<std::string, std::string>>> bySection;
        for (const auto& [k, v] : values) {
            const auto dot = k.find('.');
            if (dot == std::string::npos)
                continue;
            bySection[k.substr(0, dot)].emplace_back(k.substr(dot + 1), v);
        }
        bool first = true;
        for (auto& [section, entries] : bySection) {
            if (!first)
                f << "\n";
            first = false;
            f << "[" << section << "]\n";
            std::sort(entries.begin(), entries.end());
            for (auto& [k, v] : entries)
                f << k << "=" << v << "\n";
        }
    }

    const std::string* Get(const std::string& key) const {
        auto it = values.find(key);
        return it == values.end() ? nullptr : &it->second;
    }
    void Set(const std::string& key, std::string val) {
        values[key] = std::move(val);
    }
};

constexpr const char* kSection = "Display";
constexpr const char* kIoSection = "IO";

std::string KeyOf(const char* k) {
    return std::string(kSection) + "." + k;
}

std::string IoKeyOf(const char* k) {
    return std::string(kIoSection) + "." + k;
}

// Locale-independent integer → string. Avoids ostringstream / std::to_string
// thousands-separator surprises if the global C++ locale gets imbued.
template <typename T> std::string ToString(T v) {
    char buf[32];
    auto [ptr, ec] = std::to_chars(buf, buf + sizeof(buf), v);
    if (ec != std::errc{})
        return {};
    return std::string(buf, ptr);
}

// Same idea for floats — std::to_chars(double) writes shortest round-trip.
// We can't pass `%.3f` style precision with the default overload, so use the
// explicit precision overload (C++17 for ints, C++20 for floats; libstdc++
// has it from GCC 11, libc++ from 14).
inline std::string FloatToString(double v, int precision = 3) {
    char buf[64];
    auto [ptr, ec] = std::to_chars(buf, buf + sizeof(buf), v,
                                   std::chars_format::fixed, precision);
    if (ec != std::errc{})
        return {};
    return std::string(buf, ptr);
}

// Parse helpers use std::from_chars (locale-independent) so a user with a
// non-C LC_NUMERIC (e.g. fr_FR with comma decimal) round-trips correctly.
// from_chars's int overload accepts an optional 0x prefix only via base=16,
// so we sniff that ourselves to keep the BackgroundColor "0xRRGGBB" syntax.
bool ParseInt(const std::string& s, i32& out) {
    if (s.empty())
        return false;
    const char* first = s.data();
    const char* last = s.data() + s.size();
    int base = 10;
    if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        first += 2;
        base = 16;
    }
    long v = 0;
    auto [ptr, ec] = std::from_chars(first, last, v, base);
    if (ec != std::errc{} || ptr == first)
        return false;
    out = static_cast<i32>(v);
    return true;
}
// Minimal locale-independent float parser. We sidestep std::from_chars
// here because Apple's libc++ marks the float overload unavailable on
// every macOS deployment target as of Xcode 15 (despite from_chars
// being standardised in C++17). Our settings file only ever stores
// simple decimal values (`1.5`, `-0.123`, `12.34`) — no scientific
// notation, no hex floats, no NaN — so a hand-rolled parser is both
// adequate and the same locale-neutral guarantee.
bool ParseFloat(const std::string& s, f32& out) {
    if (s.empty())
        return false;
    const char* p = s.c_str();
    bool neg = false;
    if (*p == '-') { neg = true; ++p; }
    else if (*p == '+') { ++p; }

    double v = 0.0;
    bool gotDigit = false;
    while (*p >= '0' && *p <= '9') {
        v = v * 10.0 + static_cast<double>(*p - '0');
        ++p;
        gotDigit = true;
    }
    if (*p == '.') {
        ++p;
        double scale = 0.1;
        while (*p >= '0' && *p <= '9') {
            v += static_cast<double>(*p - '0') * scale;
            scale *= 0.1;
            ++p;
            gotDigit = true;
        }
    }
    if (!gotDigit)
        return false;
    out = static_cast<f32>(neg ? -v : v);
    return true;
}
bool ParseBool(const std::string& s, bool& out) {
    if (s == "1" || s == "true" || s == "TRUE") {
        out = true;
        return true;
    }
    if (s == "0" || s == "false" || s == "FALSE") {
        out = false;
        return true;
    }
    return false;
}

} // namespace

void LoadStartupSettingsFromIni(RenderService& service) {
    IniMap ini;
    ini.Load(SettingsIniPath());

    if (auto* s = ini.Get(KeyOf("GraphicsDebug"))) {
        bool v = false;
        if (ParseBool(*s, v))
            service.Settings().SetGraphicsDebug(v);
    }
    if (auto* s = ini.Get(KeyOf("DefaultBackend"))) {
        using gfx::GfxApi;
        if (*s == "d3d11" || *s == "D3D11")
            service.Settings().SetDefaultBackend(GfxApi::D3D11);
        else if (*s == "d3d12" || *s == "D3D12")
            service.Settings().SetDefaultBackend(GfxApi::D3D12);
        else if (*s == "vulkan" || *s == "Vulkan" || *s == "VULKAN")
            service.Settings().SetDefaultBackend(GfxApi::Vulkan);
        else if (*s == "webgpu" || *s == "WebGPU" || *s == "WEBGPU")
            service.Settings().SetDefaultBackend(GfxApi::WebGPU);
    }
    if (auto* s = ini.Get(KeyOf("PreferredDevice")); s && !s->empty()) {
        service.Settings().SetPreferredDevice(*s);
    }
}

void LoadSettingsIni(RenderService& service, bool& loopNonLoopingPolicy) {
    IniMap ini;
    ini.Load(SettingsIniPath());

    if (auto* s = ini.Get(KeyOf("BackgroundColor"))) {
        i32 v = 0;
        if (ParseInt(*s, v)) {
            const u32 c = static_cast<u32>(v);
            service.Settings().SetBackgroundColor(static_cast<u8>(c & 0xFF),
                                                  static_cast<u8>((c >> 8) & 0xFF),
                                                  static_cast<u8>((c >> 16) & 0xFF));
        }
    }
    if (auto* s = ini.Get(KeyOf("Exposure"))) {
        f32 v = 0;
        if (ParseFloat(*s, v))
            service.Settings().SetTonemapExposure(std::clamp(v, 0.0f, 3.0f));
    }
    if (auto* s = ini.Get(KeyOf("SoundVolume"))) {
        f32 v = 0;
        if (ParseFloat(*s, v))
            service.Sound().SetVolume(std::clamp(v, 0.0f, 1.0f));
    }
    if (auto* s = ini.Get(KeyOf("LoopNonLooping"))) {
        bool v = false;
        if (ParseBool(*s, v))
            loopNonLoopingPolicy = v;
    }

    {
        DisplayFlags df = service.Settings().GetDisplayFlags();
        bool dirty = false;
        auto loadFlag = [&](const char* key, bool& field) {
            if (auto* s = ini.Get(KeyOf(key))) {
                bool v = false;
                if (ParseBool(*s, v)) {
                    field = v;
                    dirty = true;
                }
            }
        };
        loadFlag("ShowGrid", df.showGrid);
        loadFlag("ShowParticles", df.showParticles);
        loadFlag("ShowRibbons", df.showRibbons);
        loadFlag("ShowEvents", df.showEvents);
        loadFlag("ShowCollisions", df.showCollisions);
        loadFlag("ShowLights", df.showLights);
        if (dirty)
            service.Settings().SetDisplayFlags(df);
    }

    if (auto* s = ini.Get(KeyOf("LightingMode"))) {
        i32 v = 0;
        if (ParseInt(*s, v) && v >= 0 && v <= 2)
            service.Settings().SetLightingMode(static_cast<LightingMode>(v));
    }
    if (auto* s = ini.Get(KeyOf("HdDebugMode"))) {
        i32 v = 0;
        if (ParseInt(*s, v) && v >= 0 && v <= 7)
            service.Settings().SetHdDebugMode(v);
    }
    if (auto* s = ini.Get(KeyOf("LodOverride"))) {
        i32 v = 0;
        if (ParseInt(*s, v) && v >= -1 && v <= 3)
            service.Settings().SetLodOverride(v);
    }
    if (auto* s = ini.Get(KeyOf("Tileset"))) {
        i32 v = 0;
        const i32 n = static_cast<i32>(io::Tileset::Count);
        if (ParseInt(*s, v) && v >= 0 && v < n)
            service.Replaceables().SetTileset(static_cast<io::Tileset>(v));
    }
    if (auto* s = ini.Get(KeyOf("IblMode"))) {
        i32 v = 0;
        if (ParseInt(*s, v) && v >= 0 && v <= static_cast<i32>(IblMode::Sunset) &&
            static_cast<IblMode>(v) != service.Settings().GetIblMode()) {
            service.Settings().SetIblMode(static_cast<IblMode>(v));
        }
    }
    if (auto* s = ini.Get(KeyOf("ShadowCascades"))) {
        i32 v = 0;
        if (ParseInt(*s, v) && v >= 0 && v <= 3) {
            if (auto* shadow = service.GetShadowService()) {
                shadow::ShadowParams p = shadow->Params();
                p.enabled = (v > 0);
                p.cascadeCount = (v > 0) ? v : 1;
                shadow->SetParams(p);
            }
        }
    }
    if (auto* dnc = service.GetDncService()) {
        if (auto* s = ini.Get(KeyOf("TimeOfDay"))) {
            f32 v = 0;
            if (ParseFloat(*s, v))
                dnc->SetTimeOfDay(v);
        }
        if (auto* s = ini.Get(KeyOf("AnimateTod"))) {
            bool v = false;
            if (ParseBool(*s, v))
                dnc->SetTodScale(v ? 1.0f : 0.0f);
        }
        if (auto* s = ini.Get(KeyOf("DncModel")); s && !s->empty() && *s != dnc->UnitMdlPath()) {
            dnc->SetUnitMdl(*s);
        }
    }
}

void SaveSettingsIni(const RenderService& service, bool loopNonLoopingPolicy) {
    const fs::path path = SettingsIniPath();
    // Round-trip: load existing values first so unrelated keys (older
    // settings, future additions) don't get nuked when a single setting
    // changes.
    IniMap ini;
    ini.Load(path);

    {
        // BackgroundColor stays in `0xRRGGBB` hex via snprintf — printf's
        // %x is locale-neutral (no decimals involved).
        char buf[16];
        std::snprintf(buf, sizeof(buf), "0x%08X",
                      static_cast<u32>(service.Settings().BackgroundColorRaw()));
        ini.Set(KeyOf("BackgroundColor"), buf);
    }
    ini.Set(KeyOf("Exposure"), FloatToString(service.Settings().GetTonemapExposure()));
    ini.Set(KeyOf("SoundVolume"), FloatToString(service.Sound().GetVolume()));
    ini.Set(KeyOf("LoopNonLooping"), loopNonLoopingPolicy ? "1" : "0");
    ini.Set(KeyOf("GraphicsDebug"), service.Settings().GraphicsDebug() ? "1" : "0");

    {
        const char* name = "d3d12";
        switch (service.Settings().DefaultBackend()) {
        case gfx::GfxApi::D3D11:
            name = "d3d11";
            break;
        case gfx::GfxApi::D3D12:
            name = "d3d12";
            break;
        case gfx::GfxApi::Vulkan:
            name = "vulkan";
            break;
        case gfx::GfxApi::WebGPU:
            name = "webgpu";
            break;
        }
        ini.Set(KeyOf("DefaultBackend"), name);
    }
    ini.Set(KeyOf("PreferredDevice"), service.Settings().PreferredDevice());

    {
        const DisplayFlags df = service.Settings().GetDisplayFlags();
        auto saveFlag = [&](const char* key, bool v) { ini.Set(KeyOf(key), v ? "1" : "0"); };
        saveFlag("ShowGrid", df.showGrid);
        saveFlag("ShowParticles", df.showParticles);
        saveFlag("ShowRibbons", df.showRibbons);
        saveFlag("ShowEvents", df.showEvents);
        saveFlag("ShowCollisions", df.showCollisions);
        saveFlag("ShowLights", df.showLights);
    }
    ini.Set(KeyOf("LightingMode"), ToString(static_cast<u32>(service.Settings().GetLightingMode())));
    ini.Set(KeyOf("HdDebugMode"), ToString(service.Settings().HdDebugMode()));
    ini.Set(KeyOf("LodOverride"), ToString(service.Settings().LodOverride()));
    ini.Set(KeyOf("Tileset"), ToString(static_cast<u32>(io::GetCurrentTileset())));
    ini.Set(KeyOf("IblMode"), ToString(static_cast<u32>(service.Settings().GetIblMode())));
    if (const auto* shadow = service.GetShadowService()) {
        const i32 cascades =
            shadow->IsEnabled() ? std::clamp(shadow->Params().cascadeCount, 1, 3) : 0;
        ini.Set(KeyOf("ShadowCascades"), ToString(cascades));
    }
    if (const auto* dnc = service.GetDncService()) {
        ini.Set(KeyOf("TimeOfDay"), FloatToString(dnc->GetTimeOfDay()));
        ini.Set(KeyOf("AnimateTod"), dnc->GetTodScale() > 0.0f ? "1" : "0");
        ini.Set(KeyOf("DncModel"), dnc->UnitMdlPath());
    }

    ini.Save(path);
}

// The MPQ list serialises as pipe-separated filenames inside one ini value;
// '|' is illegal in NTFS filenames and ASCII-printable so it never collides
// with a real entry and stays human-readable in the .ini.
static std::string JoinMpqList(const std::vector<std::string>& list) {
    std::string out;
    for (usize i = 0; i < list.size(); ++i) {
        if (i)
            out += '|';
        out += list[i];
    }
    return out;
}

static std::vector<std::string> SplitMpqList(const std::string& s) {
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

IoPathOverrides LoadIoPathOverrides() {
    IniMap ini;
    ini.Load(SettingsIniPath());
    IoPathOverrides o;
    if (auto* s = ini.Get(IoKeyOf("InstallPath")))
        o.installPath = *s;
    if (auto* s = ini.Get(IoKeyOf("IgnoreCasc"))) {
        bool v = false;
        if (ParseBool(*s, v))
            o.ignoreCasc = v;
    }
    if (auto* s = ini.Get(IoKeyOf("IgnoreMpq"))) {
        bool v = false;
        if (ParseBool(*s, v))
            o.ignoreMpq = v;
    }
    if (auto* s = ini.Get(IoKeyOf("MpqList"))) {
        o.mpqListSet = true;
        o.mpqList = SplitMpqList(*s);
    }
    return o;
}

void SaveIoPathOverrides(const IoPathOverrides& overrides) {
    const fs::path path = SettingsIniPath();
    // Round-trip existing keys so writing the IO section doesn't drop Display.
    IniMap ini;
    ini.Load(path);
    ini.Set(IoKeyOf("InstallPath"), overrides.installPath);
    ini.Set(IoKeyOf("IgnoreCasc"), overrides.ignoreCasc ? "1" : "0");
    ini.Set(IoKeyOf("IgnoreMpq"), overrides.ignoreMpq ? "1" : "0");
    // Only persist MpqList when the user has explicitly customised it —
    // omitting the key on first save means future provider versions that
    // change DefaultMpqList() are picked up automatically for these users.
    if (overrides.mpqListSet)
        ini.Set(IoKeyOf("MpqList"), JoinMpqList(overrides.mpqList));
    ini.Save(path);
}

} // namespace whiteout::flakes
