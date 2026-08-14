#pragma once

// ============================================================================
// The tiny `[Section]\nkey=value` reader/writer behind WhiteoutFlakes.ini, and
// the locale-independent scalar parsers that go with it.
//
// Header-only because two translation units share it and neither should drag
// in the other: settings_ini.cpp writes the display settings (and therefore
// links the whole RenderService), settings_io.cpp writes the content-provider
// settings (and links only the provider).
// ============================================================================

#include "whiteout/flakes/types.h"

#include <algorithm>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace whiteout::flakes::ini {

// The viewer fully owns the file (no third-party keys to preserve) so we just
// round-trip everything through this map.
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

    void Load(const std::filesystem::path& path) {
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
    // before the LAST '.'. Keys without a '.' are skipped (they'd be
    // section-less and we don't emit those today).
    //
    // Last rather than first so a dotted section name round-trips: the
    // per-game IO overrides live in `[IO.Wow]` / `[IO.Sc2]`, which Load
    // already reads back as `IO.Wow.InstallPath`. No key part contains a dot,
    // so this is identical to the old split for everything else.
    void Save(const std::filesystem::path& path) const {
        std::ofstream f(path, std::ios::trunc);
        if (!f)
            return;
        // Ordered so the file diff stays stable across saves regardless of
        // unordered_map's iteration order.
        std::map<std::string, std::vector<std::pair<std::string, std::string>>> bySection;
        for (const auto& [k, v] : values) {
            const auto dot = k.rfind('.');
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

// Locale-independent integer → string. Avoids ostringstream / std::to_string
// thousands-separator surprises if the global C++ locale gets imbued.
template <typename T>
inline std::string ToString(T v) {
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
    auto [ptr, ec] = std::to_chars(buf, buf + sizeof(buf), v, std::chars_format::fixed, precision);
    if (ec != std::errc{})
        return {};
    return std::string(buf, ptr);
}

// Parse helpers use std::from_chars (locale-independent) so a user with a
// non-C LC_NUMERIC (e.g. fr_FR with comma decimal) round-trips correctly.
// from_chars's int overload accepts an optional 0x prefix only via base=16,
// so we sniff that ourselves to keep the BackgroundColor "0xRRGGBB" syntax.
inline bool ParseInt(const std::string& s, i32& out) {
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
inline bool ParseFloat(const std::string& s, f32& out) {
    if (s.empty())
        return false;
    const char* p = s.c_str();
    bool neg = false;
    if (*p == '-') {
        neg = true;
        ++p;
    } else if (*p == '+') {
        ++p;
    }

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

inline bool ParseBool(const std::string& s, bool& out) {
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

} // namespace whiteout::flakes::ini
