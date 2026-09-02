#include "renderer/profiles/diablo3/d3_character_appearance.h"

#include "io/d3/d3_item_registry.h"
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

// The engine look category of each armour slot (g_VisualSlotToLookCategory +
// g_LookCategoryNames, retail: TRS 2, GLV 5, BTS 7, LEG 9).
u32 SlotCategory(d3n::LookSlot s) {
    switch (s) {
    case d3n::LookSlot::Torso:
        return 2;
    case d3n::LookSlot::Gloves:
        return 5;
    case d3n::LookSlot::Boots:
        return 7;
    case d3n::LookSlot::Legs:
        return 9;
    default:
        return 0;
    }
}

// What a look value is made of, read back off the value table: the display
// pieces of an exact value (weight class, variant letter, CLS family).
struct LookValueInfo {
    d3n::ArmourWeight weight;
    char variant;
    bool cls;
};
LookValueInfo InfoForLookValue(u32 v) {
    using AW = d3n::ArmourWeight;
    switch (v) {
    case 0: return {AW::Naked, 0, false};
    case 1: return {AW::Light, 'A', false};
    case 2: return {AW::Light, 'B', false};
    case 7: return {AW::Light, 'C', false};
    case 3: return {AW::Medium, 'A', false};
    case 4: return {AW::Medium, 'B', false};
    case 8: return {AW::Medium, 'C', false};
    case 5: return {AW::Heavy, 'A', false};
    case 6: return {AW::Heavy, 'B', false};
    case 9: return {AW::Heavy, 'C', false};
    default:
        break;
    }
    if (v >= 10 && v <= 18) {
        const u32 w = (v - 10) / 3;
        const AW weight = w == 0 ? AW::Light : w == 1 ? AW::Medium : AW::Heavy;
        return {weight, static_cast<char>('A' + (v - 10) % 3), true};
    }
    return {AW::Unknown, 0, false};
}

// Display rank: naked, then LIT/MED/HVY each A/B/C, then the CLS family in
// the same shape. The raw values interleave (LIT is 1,2,7), so a value sort
// would shuffle the character sheet.
u32 LookValueRank(u32 v) {
    const LookValueInfo info = InfoForLookValue(v);
    const u32 w = info.weight == d3n::ArmourWeight::Light    ? 0
                  : info.weight == d3n::ArmourWeight::Medium ? 1
                                                             : 2;
    const u32 var = info.variant ? static_cast<u32>(info.variant - 'A') : 0;
    if (info.weight == d3n::ArmourWeight::Naked) return 0;
    return 1 + (info.cls ? 9 : 0) + 3 * w + var;
}

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

    // One pass over the emitted geosets, sorting each into the slot the
    // ENGINE would flip it under, or into the pile nothing claims. Membership
    // is the engine's own test — `matchesLook`'s case-sensitive substring on
    // the shape name — so an oddly-spelled name lands exactly where retail
    // would put it; `parseGeosetName` is kept for labels and for hair, whose
    // switching is the separate 0x10404 path. Emission order is ascending, so
    // every item's geoset list comes out sorted for free.
    const usize count = adapter.EmittedSubObjects().size();
    for (usize g = 0; g < count; ++g) {
        const d3n::SubObject* sub = adapter.SubObjectAt(g);
        if (!sub)
            continue;
        const d3n::GeosetName name = d3n::parseGeosetName(*sub);
        const bool hair = name.parsed && name.slot == d3n::LookSlot::Hair;

        auto slotFor = [&](d3n::LookSlot ls) -> D3WardrobeSlot* {
            for (auto& s : w.slots) {
                if (s.slot == ls)
                    return &s;
            }
            w.slots.push_back(D3WardrobeSlot{ls, SlotName(ls), {}, 0, 0});
            return &w.slots.back();
        };

        if (hair) {
            // Hair groups by the whole token. Its entries (`Hair_NKD`,
            // `Hair_HLM1`, `Hair_HLM2`, `N_Hair`) carry no weight to key on
            // and are one-of-four helmet cutaways of a single mesh.
            D3WardrobeSlot* slot = slotFor(d3n::LookSlot::Hair);
            const std::string key(name.token);
            auto it = std::find_if(slot->items.begin(), slot->items.end(),
                                   [&](const D3WardrobeItem& i) { return i.token == key; });
            if (it == slot->items.end()) {
                D3WardrobeItem item;
                item.token = key;
                item.label = key;
                slot->items.push_back(std::move(item));
                it = slot->items.end() - 1;
            }
            it->geosets.push_back(static_cast<u32>(g));
            continue;
        }

        // The engine matches the Maya shape name (the corpus keeps the
        // pattern there); try each armour category the way ApplyLook would.
        const std::string_view matName = sub->szMaterialName;
        bool claimed = false;
        for (const d3n::LookSlot ls : {d3n::LookSlot::Torso, d3n::LookSlot::Legs,
                                       d3n::LookSlot::Boots, d3n::LookSlot::Gloves}) {
            const u32 cat = SlotCategory(ls);
            if (!d3n::matchesLookCategory(matName, cat))
                continue;
            claimed = true;
            // Which of the nineteen values draws it. At most one pattern can
            // match — the value names never contain each other under one
            // category prefix — so first match is the match.
            for (u32 v = 0; v <= 18; ++v) {
                if (!d3n::matchesLook(matName, cat, v))
                    continue;
                D3WardrobeSlot* slot = slotFor(ls);
                std::string key = std::string(d3n::lookCategoryName(cat)) + "_" +
                                  d3n::lookValueName(v);
                auto it = std::find_if(slot->items.begin(), slot->items.end(),
                                       [&](const D3WardrobeItem& i) { return i.token == key; });
                if (it == slot->items.end()) {
                    const LookValueInfo info = InfoForLookValue(v);
                    D3WardrobeItem item;
                    item.token = std::move(key);
                    item.weight = info.weight;
                    item.variant = info.variant;
                    item.lookValue = v;
                    item.label = WeightName(info.weight);
                    if (info.variant)
                        item.label += std::string(" ") + info.variant;
                    if (info.cls)
                        item.label += " (CLS)";
                    slot->items.push_back(std::move(item));
                    it = slot->items.end() - 1;
                }
                it->geosets.push_back(static_cast<u32>(g));
                claimed = true;
                v = 19; // done; a sub-object draws under one value
            }
            break; // one category token per name; the first is the engine's
        }
        if (claimed)
            continue;

        // A death body, a skill mesh, the merged oneBatch LOD, or one of the
        // five shape names that do not parse at all. The token is the better
        // label where there is one — `MonkM_decap_geo` says more than the
        // material it shares with three other meshes.
        D3WardrobeExtra ex;
        ex.name = name.parsed && !name.token.empty() ? std::string(name.token) : sub->szName;
        ex.geoset = static_cast<u32>(g);
        w.extras.push_back(std::move(ex));
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
                                 return LookValueRank(a.lookValue) < LookValueRank(b.lookValue);
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
        const u32 sel = (i < e->selection.item.size()) ? e->selection.item[i] : 0;
        if (sel == kNoneItem)
            continue; // a hair style this rig cannot spell: nothing draws
        const u32 pick = sel < slot.items.size() ? sel : 0;
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

    // The dye rows, stamped per armour slot onto the slot's drawn geosets the
    // way ActorModel_ApplyLook writes state[13] for the collected set. Dye 1
    // never lands here: the hidden mechanism resolved it to the naked look
    // above, and naked skin takes no dye.
    std::vector<i32> dyes(count, 0);
    for (usize i = 0; i < w.slots.size(); ++i) {
        i32 visual = -1;
        switch (w.slots[i].slot) {
        case d3n::LookSlot::Torso:  visual = 1; break;
        case d3n::LookSlot::Boots:  visual = 2; break;
        case d3n::LookSlot::Gloves: visual = 3; break;
        case d3n::LookSlot::Legs:   visual = 7; break;
        default: break;
        }
        if (visual < 0)
            continue;
        const i32 dye = e->outfit.slots[static_cast<usize>(visual)].dyeType;
        if (dye < d3n::kDyeFirst || dye > d3n::kDyeLast)
            continue;
        const auto& slot = w.slots[i];
        const u32 sel = (i < e->selection.item.size()) ? e->selection.item[i] : 0;
        if (sel == kNoneItem || slot.items.empty())
            continue;
        const u32 pick = sel < slot.items.size() ? sel : 0;
        for (const u32 g : slot.items[pick].geosets)
            if (g < count)
                dyes[g] = dye;
    }

    adapter.SetGeosetHidden(std::move(hidden));
    adapter.SetGeosetLooks(std::move(looks));
    adapter.SetGeosetDyes(std::move(dyes));
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

namespace {

// The wardrobe's LookSlot for an armour visual slot; Unknown for the rest.
d3n::LookSlot LookSlotForVisual(d3n::EVisualSlot slot) {
    switch (slot) {
    case d3n::EVisualSlot::Torso:
        return d3n::LookSlot::Torso;
    case d3n::EVisualSlot::Feet:
        return d3n::LookSlot::Boots;
    case d3n::EVisualSlot::Hands:
        return d3n::LookSlot::Gloves;
    case d3n::EVisualSlot::Legs:
        return d3n::LookSlot::Legs;
    default:
        return d3n::LookSlot::Unknown;
    }
}

} // namespace

void D3CharacterAppearance::ApplyOutfitArmour(Entry& e, const io::D3ModelAdapter& adapter,
                                              d3n::EVisualSlot slot) {
    const d3n::LookSlot ls = LookSlotForVisual(slot);
    const i32 si = SlotIndex(e.wardrobe, ls);
    if (si < 0)
        return;
    auto& wslot = e.wardrobe.slots[static_cast<usize>(si)];
    const auto s = static_cast<usize>(slot);
    const auto& oslot = e.outfit.slots[s];

    // ActorModel_ApplyItemLookForSlot (retail 0x750DE0): the default look
    // first — value 0, look "A" — then the item's two tags override it.
    // Dye 1 short-circuits to the default: that IS the hidden mechanism.
    u32 value = d3n::kDefaultLookValue;
    u32 lookHash = d3n::lookNameHash33(d3n::kDefaultLookName);
    if (e.itemActors[s] && oslot.itemGbid != -1 && oslot.dyeType != d3n::kDyeHidden) {
        const d3n::ItemLook look = d3n::itemLook(*e.itemActors[s]);
        if (look.lookValue)
            value = *look.lookValue;
        if (look.lookName)
            lookHash = *look.lookName;
    }

    // The fallback chain, exactly as ActorModel_ApplyLook runs it: the exact
    // value, then one simplification, then naked.
    auto pick = [&](u32 want) -> i32 {
        for (usize i = 0; i < wslot.items.size(); ++i) {
            if (wslot.items[i].lookValue == want && !wslot.items[i].geosets.empty())
                return static_cast<i32>(i);
        }
        return -1;
    };
    i32 itemIndex = pick(value);
    if (itemIndex < 0)
        itemIndex = pick(d3n::fallbackLookValue(value));
    if (itemIndex < 0)
        itemIndex = pick(0);
    if (itemIndex < 0)
        itemIndex = 0;

    // The look-name hash resolves against the appearance's look table the way
    // Appearance_FindLookIndexById does: first look on a miss, never -1.
    u32 lookIndex = 0;
    const auto& looks = adapter.Looks();
    for (usize i = 0; i < looks.size(); ++i) {
        if (d3n::lookNameHash33(looks[i]) == lookHash) {
            lookIndex = static_cast<u32>(i);
            break;
        }
    }

    e.selection.item[static_cast<usize>(si)] = static_cast<u32>(itemIndex);
    e.selection.look[static_cast<usize>(si)] = lookIndex;
}

bool D3CharacterAppearance::SetOutfitItem(const io::D3ModelAdapter& adapter,
                                          d3n::EVisualSlot slot, const io::D3ItemRecord* item,
                                          std::shared_ptr<const d3n::Actor> itemActor) {
    Entry* e = EntryFor(adapter);
    const auto s = static_cast<usize>(slot);
    if (!e || !e->wardrobe.isCharacter || s >= std::size(e->outfit.slots))
        return false;
    e->outfit.slots[s].itemGbid = item ? static_cast<i32>(item->gbid) : -1;
    e->itemActors[s] = item ? std::move(itemActor) : nullptr;
    e->itemTypes[s] = item ? item->gbidItemType : 0u;
    if (LookSlotForVisual(slot) != d3n::LookSlot::Unknown)
        ApplyOutfitArmour(*e, adapter, slot);

    if (slot == d3n::EVisualSlot::Head)
        ApplyOutfitHair(*e);
    return true;
}

void D3CharacterAppearance::ApplyOutfitHair(Entry& e) {
    // A head item picks the hair cutaway through tag 0x10404, the dedicated
    // path ActorModel_UpdateAttachedItemVisual runs for slot 0 only. No helm
    // (or a hidden one) restores NKD.
    //
    // The tag lives on the RESOLVED attach model — the per-class art —
    // not on the generic item Actor: retail reads Actor_GetTagMapValue off
    // ActorModel_GetItemAttachModelSno's result (0x750FC0), and measured,
    // 618 of the 699 helm-family actors carry a nonzero style only there
    // while the item Actors all say 0.
    const auto s = static_cast<usize>(d3n::EVisualSlot::Head);
    u32 style = 0;
    if (e.itemActors[s] && e.outfit.slots[s].dyeType != d3n::kDyeHidden) {
        const d3n::Actor* src = e.itemActors[s].get();
        std::shared_ptr<const d3n::Actor> attach;
        const d3n::EquipVisual v =
            d3n::resolveEquip(*src, d3n::itemTypeTraits(e.itemTypes[s]), d3n::EVisualSlot::Head,
                              e.outfit.cls, e.outfit.gender, e.outfit.sheathed);
        if (v.attachActorSno > 0 && v.attachActorSno != src->dwSnoId && actorSource_) {
            if ((attach = actorSource_(v.attachActorSno)))
                src = attach.get();
        }
        style = d3n::tagMapValue(src->arTagMap, d3n::kTagItemHairStyle).value_or(0);
    }
    const i32 hi = SlotIndex(e.wardrobe, d3n::LookSlot::Hair);
    if (hi < 0)
        return;
    // ApplyHairStyle (retail 0x764070) shows the one Hair_* sub-object whose
    // suffix matches the style and HIDES every other — so a style nothing in
    // this rig spells means no hair at all. That is how a BALD helm removes a
    // monk's beard: the beard is his Hair_NKD, and no Hair_BALD exists.
    const auto& hairItems = e.wardrobe.slots[static_cast<usize>(hi)].items;
    const char* want = d3n::hairStyleName(static_cast<d3n::HairStyle>(style));
    u32 pick = kNoneItem;
    for (usize i = 0; i < hairItems.size(); ++i) {
        if (hairItems[i].token.find(want) != std::string::npos) {
            pick = static_cast<u32>(i);
            break;
        }
        // NKD's cutaway also ships as a bare `N_Hair` token in some rigs;
        // treat it as the naked style's spelling.
        if (style == 0 && pick == kNoneItem && hairItems[i].token == "N_Hair")
            pick = static_cast<u32>(i);
    }
    e.selection.item[static_cast<usize>(hi)] = pick;
}

void D3CharacterAppearance::SetOutfitBody(const io::D3ModelAdapter& adapter,
                                          d3n::PlayerClass cls, d3n::Gender gender) {
    if (Entry* e = EntryFor(adapter)) {
        e->outfit.cls = cls;
        e->outfit.gender = gender;
    }
}

void D3CharacterAppearance::SetOutfitSheathed(const io::D3ModelAdapter& adapter, bool sheathed) {
    if (Entry* e = EntryFor(adapter))
        e->outfit.sheathed = sheathed;
}

std::vector<D3OutfitAttachment> D3CharacterAppearance::OutfitAttachments(
    const io::D3ModelAdapter& adapter) const {
    std::vector<D3OutfitAttachment> out;
    const Entry* e = EntryFor(adapter);
    if (!e)
        return out;
    for (const auto slot : {d3n::EVisualSlot::Head, d3n::EVisualSlot::RightHand,
                            d3n::EVisualSlot::LeftHand, d3n::EVisualSlot::Shoulders}) {
        const auto s = static_cast<usize>(slot);
        const auto& oslot = e->outfit.slots[s];
        // gbid -1 and dye 1 are one case in the engine
        // (ActorModel_UpdateAttachedItemVisual groups them): nothing attached.
        if (oslot.itemGbid == -1 || oslot.dyeType == d3n::kDyeHidden || !e->itemActors[s])
            continue;
        const d3n::EquipVisual v =
            d3n::resolveEquip(*e->itemActors[s], d3n::itemTypeTraits(e->itemTypes[s]), slot,
                              e->outfit.cls, e->outfit.gender, e->outfit.sheathed);
        if (!v.drawn)
            continue;
        D3OutfitAttachment a;
        a.visualSlot = static_cast<i32>(slot);
        a.itemGbid = oslot.itemGbid;
        a.actorSno = v.attachActorSno;
        a.hardpoint = v.hardpoint;
        a.dyeType = oslot.dyeType;
        a.sheathed = d3n::isSheathHardpoint(v.hardpoint);
        if (a.actorSno > 0)
            out.push_back(a);
        if (v.secondAttach && v.secondAttachActorSno > 0) {
            D3OutfitAttachment b = a;
            b.actorSno = v.secondAttachActorSno;
            b.hardpoint = "HP_right_shoulderPad";
            out.push_back(b);
        }
    }
    return out;
}

void D3CharacterAppearance::SetOutfitDye(const io::D3ModelAdapter& adapter,
                                         d3n::EVisualSlot slot, i32 dyeType) {
    Entry* e = EntryFor(adapter);
    const auto s = static_cast<usize>(slot);
    if (!e || s >= std::size(e->outfit.slots))
        return;
    e->outfit.slots[s].dyeType = dyeType;
    if (LookSlotForVisual(slot) != d3n::LookSlot::Unknown)
        ApplyOutfitArmour(*e, adapter, slot);
    // Dye 1 on the helm is "no helm" for the hair too.
    if (slot == d3n::EVisualSlot::Head)
        ApplyOutfitHair(*e);
}

D3Outfit D3CharacterAppearance::OutfitOf(const io::D3ModelAdapter& adapter) const {
    const Entry* e = EntryFor(adapter);
    return e ? e->outfit : D3Outfit{};
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
