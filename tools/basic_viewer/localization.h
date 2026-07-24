// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file localization.h
 * @brief Lightweight UI string localization (i18n) for the viewer.
 *
 * User-facing strings are looked up by stable keys (e.g. "menu.file.open") via
 * tr(). Catalogs are plain UTF-8 INI files, one per language, loaded from a
 * `lang/` directory next to the executable. English is always loaded as a
 * fallback, so a missing key resolves to the English text and, failing that, to
 * the key itself (which keeps untranslated strings visible and debuggable).
 *
 * Switching language only swaps the in-memory catalog. CJK glyphs live in the
 * bundled Noto fonts, which are baked into the ImGui atlas up front (the engine
 * ImGui renderer uses the legacy single-atlas path), so no restart is needed.
 */

#include "whiteout/flakes/types.h"

#include <span>
#include <string>
#include <string_view>
#include <unordered_map>

namespace whiteout::flakes::i18n {

/// Supported UI languages. The integer values are persisted indirectly via the
/// stable string codes (see languageCode()), not the enum value, so the order
/// here is free to change.
enum class Language : i32 {
    English = 0,
    Spanish,
    German,
    French,
    Italian,
    PortugueseBrazilian,
    Russian,
    ChineseSimplified,
    ChineseTraditional,
    Japanese,
    Korean,
};

/// A selectable language: its stable file/config code and its native name.
struct LanguageEntry {
    Language lang;
    const char* code;    ///< Stable code: "en", "es", "de", "fr", "zh".
    const char* endonym; ///< Native display name for the menu, e.g. "Español".
};

/// All selectable languages, in display order.
std::span<const LanguageEntry> languages();

/// Stable code for @p lang (e.g. "en"). Used for the catalog file name and the
/// persisted preference value.
const char* languageCode(Language lang);

/// Parse a language code back to the enum, or @p fallback if unrecognised.
Language languageFromCode(std::string_view code, Language fallback = Language::English);

/// Holds the active and English-fallback string catalogs and resolves keys.
/// Single instance accessed via instance(); lives for the program duration.
class Localizer {
public:
    static Localizer& instance();

    Localizer(const Localizer&) = delete;
    Localizer& operator=(const Localizer&) = delete;

    /// Point the localizer at @p langDir (where the `<code>.ini` files live) and
    /// load @p lang as the active catalog plus the English fallback catalog.
    void load(const std::string& langDir, Language lang);

    /// Switch the active language, reloading its catalog from the stored dir.
    void setLanguage(Language lang);

    /// Currently active language.
    Language current() const {
        return current_;
    }

    /// Translate @p key: active catalog -> English fallback -> @p key verbatim.
    /// The returned pointer is valid until the next load()/setLanguage().
    const char* tr(const char* key) const;

private:
    Localizer() = default;

    /// Transparent hashing so we can look up by const char* / string_view
    /// without allocating a temporary std::string per call.
    struct StrHash {
        using is_transparent = void;
        size_t operator()(std::string_view s) const noexcept {
            return std::hash<std::string_view>{}(s);
        }
    };
    using StrMap = std::unordered_map<std::string, std::string, StrHash, std::equal_to<>>;

    void loadInto(StrMap& map, Language lang) const;

    std::string dir_;
    Language current_ = Language::English;
    StrMap active_;
    StrMap english_;
};

/// Terse free-function form: i18n::tr("menu.file.open").
inline const char* tr(const char* key) {
    return Localizer::instance().tr(key);
}

} // namespace whiteout::flakes::i18n
