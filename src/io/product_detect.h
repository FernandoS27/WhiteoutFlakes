#pragma once

// Build-product string → ProductId.
//
// There is no ready-made product code to read. `casc::StorageProduct` is
// `{name, version, buildId}`, and `name` is `buildConfig.buildProduct` — the
// build config's `build-product` field, whose values are Blizzard's internal
// product names (`WoW`, `War3`, `Sc2`, …), not the lowercase CDN codes.
//
// The CDN codes (`wow`, `w3`, `s2`) do exist, but they live on
// `OnlineOpenOptions::product` and the `.build.info` `Product` column, which
// describe what to *open*, not what was opened. Both spellings are matched
// here because an online storage and a local one can reach this with either,
// and getting it wrong picks the wrong render profile for the whole scene.
//
// Unrecognised names return Neutral rather than guessing. A scene with no
// product yet is a real state (nothing loaded), so there is no need for a
// wrong answer to stand in for it.

#include "whiteout/flakes/enums.h"

#include <cctype>
#include <string>
#include <string_view>

namespace whiteout::flakes::io {

/// @brief Normalise a build-product / CDN product string to a @ref ProductId.
///        Case-insensitive; returns @ref ProductId::Neutral for anything not
///        listed.
inline ProductId ProductIdFromBuildProduct(std::string_view name) {
    std::string k;
    k.reserve(name.size());
    for (char c : name)
        k += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    struct Entry {
        std::string_view key;
        ProductId id;
    };
    // `_beta` / `t` (test) / `_classic` variants are separate products on the
    // CDN and separate build-product strings, so each spelling is listed
    // rather than prefix-matched — "w3" must not swallow a future "w3x" that
    // turns out to be something else.
    static constexpr Entry kTable[] = {
        // Warcraft III
        {"war3", ProductId::Wc3},   {"w3", ProductId::Wc3},
        {"w3t", ProductId::Wc3},    {"w3b", ProductId::Wc3},
        // World of Warcraft
        {"wow", ProductId::Wow},    {"wowt", ProductId::Wow},
        {"wow_beta", ProductId::Wow}, {"wow_classic", ProductId::Wow},
        {"wow_classic_era", ProductId::Wow},
        {"wowxptr", ProductId::Wow},
        // StarCraft II
        {"sc2", ProductId::Sc2},    {"s2", ProductId::Sc2},
        {"s2t", ProductId::Sc2},    {"s2b", ProductId::Sc2},
        // Heroes of the Storm, which has no ProductId of its own on purpose.
        // The enum selects a *render profile*, and Heroes ships `.m3` through
        // the same frame StarCraft II does — that is why the profile is named
        // sc2_heroes. Splitting them is a bound-enum change, so it waits for
        // the phase that regenerates bindings anyway (P11) and for a
        // difference that actually needs expressing.
        {"hero", ProductId::Sc2},   {"heroes", ProductId::Sc2},
        {"herot", ProductId::Sc2},
        // Diablo III. "diablo3" is the measured build-product; "d3" is the
        // CDN code and the build-uid prefix.
        {"diablo3", ProductId::D3}, {"d3", ProductId::D3},
        {"d3t", ProductId::D3},     {"d3b", ProductId::D3},
    };
    for (const Entry& e : kTable) {
        if (k == e.key)
            return e.id;
    }
    return ProductId::Neutral;
}

} // namespace whiteout::flakes::io
