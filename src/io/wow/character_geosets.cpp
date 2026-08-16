#include "io/wow/character_geosets.h"

#include <whiteout/models/m2/m2.h>

#include <algorithm>

namespace whiteout::flakes::io::wow {

bool IsCharacterModel(const ::whiteout::m2::Model& model) {
    for (const auto& tex : model.textures) {
        if (std::find(kCharacterTextureTypes.begin(), kCharacterTextureTypes.end(), tex.type) !=
            kCharacterTextureTypes.end())
            return true;
    }
    return false;
}

std::vector<u16> DeclaredGeosets(const ::whiteout::m2::Model& model, usize profileIndex) {
    std::vector<u16> out;
    if (profileIndex >= model.skinProfiles.size())
        return out;
    const auto& skin = model.skinProfiles[profileIndex];
    out.reserve(skin.submeshes.size());
    for (const auto& sec : skin.submeshes)
        out.push_back(sec.skinSectionId);
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

CharacterGeosetSelection DefaultSelection() {
    CharacterGeosetSelection sel;
    // The bare variants, from the CCharacterComponent constructor (0x10033d2b0):
    // it seeds `m_geosets[0..19]` with `group * 100 + 1` — 1, 101, 201 … 1901 —
    // and ears with 702. Those are not accessories: 401 is the hand a glove
    // replaces, 501 the foot a boot replaces, 1301 the leg a robe replaces.
    // Dropping them does not undress a character, it dismembers one.
    for (u16 group = 0; group <= 19; ++group)
        sel.Set(group, 1);
    sel.Set(7, 2);
    // Group 20 has no slot in that array; GeosRenderPrep shows 2001 itself
    // (LABEL_61, the no-boots fall-through).
    sel.Set(20, 1);

    // Five of those the constructor never gets to keep. `RemoveItem` runs on
    // every character as it is dressed, and case 0 rewrites them from the
    // facial-hair record: `rec[4] + 100`, `rec[6] + 200`, `rec[5] + 300`,
    // `rec[7] + 1600`, `rec[8] + 1700`. Variation 0 is "none" and that is what
    // a character with no beard, no markings and no eye glow gets, so the
    // settled value is `group * 100` — an id no model declares.
    //
    // Group 17 is the one that shows: 1701 is a glow quad over the eye, and
    // leaving it on is why every character looked like a death knight.
    for (const u16 group : {1, 2, 3, 16, 17})
        sel.Set(group, -1);
    return sel;
}

std::vector<u16> VisibleGeosets(const CharacterGeosetSelection& selection,
                                std::span<const u16> declared) {
    std::vector<u16> out;
    // `SetGeometryVisible(0, 0, 1)`, before any choice is consulted.
    out.push_back(0);

    for (const u16 id : declared) {
        const u16 group = GeosetGroupOf(id);
        const u16 v = GeosetValueOf(id);

        // ChrCustomization first, and per id: an option turns off exactly the
        // geosets its own choices name and leaves every sibling alone.
        if (selection.IsControlled(id)) {
            if (selection.IsShown(id))
                out.push_back(id);
            continue;
        }

        // Then the client, for the groups it has an array slot for.
        if (group < kClientGeosetGroups) {
            if (selection.Get(group) == static_cast<i16>(v))
                out.push_back(id);
            continue;
        }

        // Then, for the modern groups, WoWModelViewer's rule. Earrings are its
        // one exclusion up here — jewellery is something a choice adds, never
        // something a bare character has. Its other, the eye glow, is group 17
        // and the client already answered for that one.
        if (group == 35)
            continue;
        if (v == 1 || group == 32)
            out.push_back(id);
    }

    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

} // namespace whiteout::flakes::io::wow
