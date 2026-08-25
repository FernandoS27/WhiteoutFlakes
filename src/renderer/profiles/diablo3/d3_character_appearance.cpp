#include "renderer/profiles/diablo3/d3_character_appearance.h"

#include "io/d3/d3_model_adapter.h"

#include <algorithm>
#include <cctype>
#include <iterator>
#include <string_view>
#include <tuple>

namespace whiteout::flakes::renderer::profiles::diablo3 {

namespace {

const char* WeightName(d3n::ArmourWeight w) {
    switch (w) {
    case d3n::ArmourWeight::Naked:
        return "Naked";
    case d3n::ArmourWeight::Light:
        return "Light";
    case d3n::ArmourWeight::Medium:
        return "Medium";
    case d3n::ArmourWeight::Heavy:
        return "Heavy";
    default:
        return "Other";
    }
}

const char* SlotName(d3n::LookSlot s) {
    switch (s) {
    case d3n::LookSlot::Torso:
        return "Torso";
    case d3n::LookSlot::Legs:
        return "Legs";
    case d3n::LookSlot::Boots:
        return "Boots";
    case d3n::LookSlot::Gloves:
        return "Gloves";
    case d3n::LookSlot::Hair:
        return "Hair";
    default:
        return "Other";
    }
}

bool ContainsNoCase(std::string_view haystack, std::string_view needle) {
    if (needle.size() > haystack.size())
        return false;
    const auto lower = [](char c) {
        return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    };
    for (usize i = 0; i + needle.size() <= haystack.size(); ++i) {
        usize j = 0;
        while (j < needle.size() && lower(haystack[i + j]) == lower(needle[j]))
            ++j;
        if (j == needle.size())
            return true;
    }
    return false;
}

// The four equipment slots, in the order a character sheet reads. Hair is
// appended after them because it is switched by the helmet rather than worn on
// its own, and putting it last keeps the four armour rows together.
constexpr d3n::LookSlot kSlotOrder[] = {
    d3n::LookSlot::Torso,
    d3n::LookSlot::Legs,
    d3n::LookSlot::Boots,
    d3n::LookSlot::Gloves,
    d3n::LookSlot::Hair,
};

} // namespace

// ---------------------------------------------------------------------------
// Reading the wardrobe out of the file
// ---------------------------------------------------------------------------

D3CharacterAppearance::Entry* D3CharacterAppearance::EntryFor(
    const io::D3ModelAdapter& adapter) const {
    const i32 sno = adapter.AppearanceSno();
    if (sno <= 0)
        return nullptr;
    if (auto it = byAppearance_.find(sno); it != byAppearance_.end())
        return &it->second;

    Entry entry;
    Wardrobe& w = entry.wardrobe;

    // One pass over the emitted geosets, sorting each into the slot its shape
    // name names or into the pile nothing claims. Emission order is ascending,
    // so every item's geoset list comes out sorted for free.
    const usize count = adapter.EmittedSubObjects().size();
    for (usize g = 0; g < count; ++g) {
        const d3n::SubObject* sub = adapter.SubObjectAt(g);
        if (!sub)
            continue;
        const d3n::GeosetName name = d3n::parseGeosetName(*sub);
        const bool armour = name.parsed && name.slot != d3n::LookSlot::Unknown &&
                            name.slot != d3n::LookSlot::Hair &&
                            name.weight != d3n::ArmourWeight::Unknown;
        const bool hair = name.parsed && name.slot == d3n::LookSlot::Hair;
        if (!armour && !hair) {
            // A death body, a skill mesh, the merged oneBatch LOD, or one of
            // the five shape names that do not parse at all. The token is the
            // better label where there is one — `MonkM_decap_geo` says more
            // than the material it shares with three other meshes.
            D3WardrobeExtra ex;
            ex.name = name.parsed && !name.token.empty() ? std::string(name.token) : sub->szName;
            ex.geoset = static_cast<u32>(g);
            w.extras.push_back(std::move(ex));
            continue;
        }

        auto* slot = [&]() -> D3WardrobeSlot* {
            for (auto& s : w.slots) {
                if (s.slot == name.slot)
                    return &s;
            }
            w.slots.push_back(D3WardrobeSlot{name.slot, SlotName(name.slot), {}, 0, 0});
            return &w.slots.back();
        }();

        // Armour groups by what an equipped item would select — weight, its
        // qualifier and its variant letter. `_Cloth` is deliberately not part
        // of the key: a skirt is part of the legs it hangs off, not a piece a
        // player could wear instead of them.
        //
        // Hair groups by the whole token instead. Its four entries
        // (`Hair_NKD`, `Hair_HLM1`, `Hair_HLM2`, `N_Hair`) carry no weight to
        // key on and are one-of-four helmet cutaways of a single mesh.
        const std::string key = armour ? (std::string(name.qualifier) + '|' +
                                          WeightName(name.weight) + '|' +
                                          (name.variant ? name.variant : '-'))
                                       : std::string(name.token);
        auto it = std::find_if(slot->items.begin(), slot->items.end(),
                               [&](const D3WardrobeItem& i) { return i.token == key; });
        if (it == slot->items.end()) {
            D3WardrobeItem item;
            item.token = key;
            item.weight = name.weight;
            item.variant = name.variant;
            item.lookValue = d3n::armourWeightBaseLookValue(name.weight);
            if (armour) {
                item.label = WeightName(name.weight);
                if (name.variant)
                    item.label += std::string(" ") + name.variant;
                if (!name.qualifier.empty())
                    item.label += " (" + std::string(name.qualifier) + ")";
            } else {
                item.label = std::string(name.token);
            }
            slot->items.push_back(std::move(item));
            it = slot->items.end() - 1;
        }
        it->geosets.push_back(static_cast<u32>(g));
    }

    // Slots in character-sheet order, and pieces within a slot in the order the
    // look value climbs: naked, light, medium, heavy. That makes item 0 the
    // default everywhere — `Appearance_GetDefaultLook` hands out value 0, which
    // is the naked mesh, and a character with nothing equipped is what the
    // game shows.
    std::stable_sort(w.slots.begin(), w.slots.end(),
                     [](const D3WardrobeSlot& a, const D3WardrobeSlot& b) {
                         const auto rank = [](d3n::LookSlot s) {
                             for (usize i = 0; i < std::size(kSlotOrder); ++i) {
                                 if (kSlotOrder[i] == s)
                                     return i;
                             }
                             return std::size(kSlotOrder);
                         };
                         return rank(a.slot) < rank(b.slot);
                     });
    for (auto& s : w.slots) {
        if (s.slot == d3n::LookSlot::Hair) {
            // No weight to rank on, so rank on the one thing that is known:
            // `Hair_NKD` is the no-helmet mesh and belongs first for the same
            // reason Naked does.
            std::stable_sort(s.items.begin(), s.items.end(),
                             [](const D3WardrobeItem& a, const D3WardrobeItem& b) {
                                 const bool an = ContainsNoCase(a.token, "NKD");
                                 const bool bn = ContainsNoCase(b.token, "NKD");
                                 if (an != bn)
                                     return an;
                                 return a.token < b.token;
                             });
        } else {
            std::stable_sort(s.items.begin(), s.items.end(),
                             [](const D3WardrobeItem& a, const D3WardrobeItem& b) {
                                 return std::tie(a.weight, a.variant, a.token) <
                                        std::tie(b.weight, b.variant, b.token);
                             });
        }
    }

    // A wardrobe, not a body: two of the four armour slots offering a choice.
    // A creature has one mesh per slot and nothing to switch, and matching on
    // the file name instead would miss every `_characterSelect` and `_FrontEnd`
    // rig — which are the same character and equally dressable.
    usize slotsWithChoice = 0;
    for (const auto& s : w.slots) {
        if (s.slot != d3n::LookSlot::Hair && s.items.size() >= 2)
            ++slotsWithChoice;
    }
    w.isCharacter = slotsWithChoice >= 2;

    entry.selection.item.assign(w.slots.size(), 0);
    entry.selection.look.assign(w.slots.size(), adapter.LookIndex());
    entry.selection.extra.assign(w.extras.size(), 0);

    return &byAppearance_.emplace(sno, std::move(entry)).first->second;
}

i32 D3CharacterAppearance::SlotIndex(const Wardrobe& w, d3n::LookSlot slot) {
    for (usize i = 0; i < w.slots.size(); ++i) {
        if (w.slots[i].slot == slot)
            return static_cast<i32>(i);
    }
    return -1;
}

// ---------------------------------------------------------------------------
// Dressing
// ---------------------------------------------------------------------------

bool D3CharacterAppearance::IsCharacter(const io::D3ModelAdapter& adapter) const {
    const Entry* e = EntryFor(adapter);
    return e && e->wardrobe.isCharacter;
}

bool D3CharacterAppearance::Apply(io::D3ModelAdapter& adapter) {
    Entry* e = EntryFor(adapter);
    if (!e || !e->wardrobe.isCharacter)
        return false;

    // Start from nothing drawing and turn on what is worn. The other direction
    // — start from everything and hide what is not — needs a rule for every
    // sub-object rather than for the chosen ones, and the unclaimed pile is
    // exactly where there is no rule to have.
    const usize count = adapter.EmittedSubObjects().size();
    std::vector<u8> hidden(count, 1);
    std::vector<u32> looks(count, adapter.LookIndex());

    const auto& w = e->wardrobe;
    for (usize i = 0; i < w.slots.size(); ++i) {
        const auto& slot = w.slots[i];
        if (slot.items.empty())
            continue;
        const u32 pick = (i < e->selection.item.size() &&
                          e->selection.item[i] < slot.items.size())
                             ? e->selection.item[i]
                             : 0;
        const u32 look = (i < e->selection.look.size()) ? e->selection.look[i] : adapter.LookIndex();
        for (const u32 g : slot.items[pick].geosets) {
            if (g >= count)
                continue;
            hidden[g] = 0;
            looks[g] = look;
        }
    }
    for (usize i = 0; i < w.extras.size(); ++i) {
        if (i < e->selection.extra.size() && e->selection.extra[i] &&
            w.extras[i].geoset < count)
            hidden[w.extras[i].geoset] = 0;
    }

    adapter.SetGeosetHidden(std::move(hidden));
    adapter.SetGeosetLooks(std::move(looks));
    return true;
}

// ---------------------------------------------------------------------------
// What a host asks about, and what it sets
// ---------------------------------------------------------------------------

std::vector<D3WardrobeSlot> D3CharacterAppearance::Slots(const io::D3ModelAdapter& adapter) const {
    const Entry* e = EntryFor(adapter);
    if (!e || !e->wardrobe.isCharacter)
        return {};
    std::vector<D3WardrobeSlot> out = e->wardrobe.slots;
    for (usize i = 0; i < out.size(); ++i) {
        out[i].selectedItem = (i < e->selection.item.size()) ? e->selection.item[i] : 0;
        out[i].lookIndex = (i < e->selection.look.size()) ? e->selection.look[i] : 0;
    }
    return out;
}

std::vector<D3WardrobeExtra> D3CharacterAppearance::Extras(
    const io::D3ModelAdapter& adapter) const {
    const Entry* e = EntryFor(adapter);
    if (!e || !e->wardrobe.isCharacter)
        return {};
    std::vector<D3WardrobeExtra> out = e->wardrobe.extras;
    for (usize i = 0; i < out.size(); ++i)
        out[i].shown = (i < e->selection.extra.size()) && e->selection.extra[i] != 0;
    return out;
}

void D3CharacterAppearance::SetItem(const io::D3ModelAdapter& adapter, d3n::LookSlot slot,
                                    u32 itemIndex) {
    Entry* e = EntryFor(adapter);
    if (!e)
        return;
    const i32 i = SlotIndex(e->wardrobe, slot);
    if (i < 0 || itemIndex >= e->wardrobe.slots[static_cast<usize>(i)].items.size())
        return;
    e->selection.item[static_cast<usize>(i)] = itemIndex;
}

void D3CharacterAppearance::SetSlotLook(const io::D3ModelAdapter& adapter, d3n::LookSlot slot,
                                        u32 lookIndex) {
    Entry* e = EntryFor(adapter);
    if (!e || lookIndex >= adapter.Looks().size())
        return;
    const i32 i = SlotIndex(e->wardrobe, slot);
    if (i < 0)
        return;
    e->selection.look[static_cast<usize>(i)] = lookIndex;
}

void D3CharacterAppearance::SetLookForAll(const io::D3ModelAdapter& adapter, u32 lookIndex) {
    Entry* e = EntryFor(adapter);
    if (!e || lookIndex >= adapter.Looks().size())
        return;
    std::fill(e->selection.look.begin(), e->selection.look.end(), lookIndex);
}

void D3CharacterAppearance::SetExtra(const io::D3ModelAdapter& adapter, u32 geoset, bool shown) {
    Entry* e = EntryFor(adapter);
    if (!e)
        return;
    for (usize i = 0; i < e->wardrobe.extras.size(); ++i) {
        if (e->wardrobe.extras[i].geoset == geoset) {
            e->selection.extra[i] = shown ? u8{1} : u8{0};
            return;
        }
    }
}

} // namespace whiteout::flakes::renderer::profiles::diablo3
