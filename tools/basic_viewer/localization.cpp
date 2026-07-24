// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "localization.h"

#include "whiteout/flakes/util/path_utf8.h"

#include <array>
#include <filesystem>
#include <fstream>

namespace whiteout::flakes::i18n {

namespace {

constexpr std::array<LanguageEntry, 11> kLanguages = {{
    {Language::English, "en", "English"},
    {Language::Spanish, "es", "Español"},
    {Language::German, "de", "Deutsch"},
    {Language::French, "fr", "Français"},
    {Language::Italian, "it", "Italiano"},
    {Language::PortugueseBrazilian, "pt-br", "Português (Brasil)"},
    {Language::Russian, "ru", "Русский"},
    {Language::ChineseSimplified, "zh", "简体中文"},
    {Language::ChineseTraditional, "zh-hant", "繁體中文"},
    {Language::Japanese, "ja", "日本語"},
    {Language::Korean, "ko", "한국어"},
}};

/// Convert backslash escape sequences in catalog values to real characters, so
/// multi-line strings can live on a single INI line: `\n` -> newline,
/// `\t` -> tab, `\\` -> backslash.
std::string unescape(std::string_view in) {
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] == '\\' && i + 1 < in.size()) {
            switch (in[i + 1]) {
            case 'n':
                out += '\n';
                ++i;
                continue;
            case 't':
                out += '\t';
                ++i;
                continue;
            case '\\':
                out += '\\';
                ++i;
                continue;
            default:
                break;
            }
        }
        out += in[i];
    }
    return out;
}

} // namespace

std::span<const LanguageEntry> languages() {
    return kLanguages;
}

const char* languageCode(Language lang) {
    for (const auto& e : kLanguages)
        if (e.lang == lang)
            return e.code;
    return "en";
}

Language languageFromCode(std::string_view code, Language fallback) {
    for (const auto& e : kLanguages)
        if (code == e.code)
            return e.lang;
    return fallback;
}

Localizer& Localizer::instance() {
    static Localizer inst;
    return inst;
}

void Localizer::loadInto(StrMap& map, Language lang) const {
    map.clear();
    if (dir_.empty())
        return;

    // dir_ is UTF-8; convert explicitly so a non-ASCII install directory still
    // resolves on Windows.
    const auto path =
        io::FsPathFromUtf8(dir_) / (std::string(languageCode(lang)) + ".ini");
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open())
        return;

    std::string line;
    bool first = true;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        // Strip a UTF-8 BOM if some editor added one to the first line.
        if (first) {
            first = false;
            if (line.size() >= 3 && static_cast<unsigned char>(line[0]) == 0xEF &&
                static_cast<unsigned char>(line[1]) == 0xBB &&
                static_cast<unsigned char>(line[2]) == 0xBF)
                line.erase(0, 3);
        }
        if (line.empty() || line[0] == '#' || line[0] == ';' || line[0] == '[')
            continue;
        const auto eq = line.find('=');
        if (eq == std::string::npos)
            continue;
        map.insert_or_assign(line.substr(0, eq), unescape(line.substr(eq + 1)));
    }
}

void Localizer::load(const std::string& langDir, Language lang) {
    dir_ = langDir;
    loadInto(english_, Language::English);
    setLanguage(lang);
}

void Localizer::setLanguage(Language lang) {
    current_ = lang;
    if (lang == Language::English)
        active_ = english_;
    else
        loadInto(active_, lang);
}

const char* Localizer::tr(const char* key) const {
    const std::string_view k{key};
    if (auto it = active_.find(k); it != active_.end())
        return it->second.c_str();
    if (auto it = english_.find(k); it != english_.end())
        return it->second.c_str();
    return key; // Fallback: show the key itself (keys are string literals).
}

} // namespace whiteout::flakes::i18n
