#include "io/d3/d3_item_registry.h"

#include "io/progress.h"
#include "whiteout/flakes/content_provider.h"

#include <whiteout/sno/d3/native/d3_native.h>

#include "whiteout/flakes/content_ref.h"

#include <algorithm>
#include <cstring>
#include <deque>
#include <fstream>
#include <memory>
#include <unordered_set>

namespace whiteout::flakes::io {

namespace {

// Every ItemType name that cracks against a shipped `gbidItemType`. The bulk
// is the exe's own display table (retail 0x1447C00); the rest are the record
// spellings the display table abbreviates (MightyWeapon1H where it shows
// MightyWeapon), found by hashing candidates against the shipped values.
// This is the OFFLINE floor: ItemTypeNames.stl, when a storage or the corpus
// snapshot offers it, closes what this list cannot know — the 44 class-
// suffixed armour spellings (Belt_Barbarian, SpiritStone_Monk, ...) and the
// D2-flavour weapon-art types — bringing coverage to 129 of the 131 shipped
// type gbids (the 2 leftovers are single non-equippable quest items).
constexpr const char* kItemTypeNames[] = {
    // clang-format off
    "Amulet", "Axe", "Axe2H", "Belt", "BloodShard", "Book", "Boots", "Bow",
    "Bracers", "ChestArmor", "Cloak", "CombatStaff", "CosmeticPennant",
    "CosmeticPet", "CosmeticPortraitFrame", "CosmeticWings", "CraftingPlan",
    "CraftingPlanGeneric", "CraftingPlanLegendary", "CraftingReagent",
    "Crossbow", "CrusaderShield", "Dagger", "EnchantressSpecial", "FistWeapon",
    "Flail", "Flail1H", "Flail2H", "FollowerSpecial", "Gem", "GenericBelt",
    "GenericBowWeapon", "GenericChestArmor", "GenericHelm", "GenericOffHand",
    "GenericRangedWeapon", "GenericSwingWeapon", "GenericThrustWeapon",
    "Gloves", "Gold", "GreaterShard", "HandXBow", "HealthPotion", "Helm",
    "HoradricReagent", "Jewel", "Junk", "Legs", "Mace", "Mace2H",
    "MightyWeapon1H", "MightyWeapon2H", "Mojo", "NecromancerOffhand",
    "NephalemCube", "Orb", "Ornament", "Platinum", "Polearm", "Quiver",
    "Ring", "Scroll",
    "ScoundrelSpecial", "Scythe", "Scythe1H", "Scythe2H", "Shard", "Shield",
    "Shoulders", "Spear", "Staff", "Sword", "Sword2H", "TemplarSpecial",
    "TieredRiftKey", "UpgradeableJewel", "VoodooMask", "WizardHat",
    // clang-format on
};

const std::unordered_map<u32, std::string_view>& TypeNamesByGbid() {
    static const auto kMap = [] {
        std::unordered_map<u32, std::string_view> m;
        for (const char* n : kItemTypeNames)
            m.emplace(d3n::gbidHash(n), n);
        return m;
    }();
    return kMap;
}

bool IsGam(std::string_view path) {
    return path.size() > 4 && (path.substr(path.size() - 4) == ".gam");
}

constexpr u16 SlotBit(d3n::EVisualSlot s) {
    return static_cast<u16>(1u << static_cast<u32>(s));
}

// The four geoset-switching slots. An armour look value works in whichever
// category the appearance ships it under — the mechanism is slot-agnostic —
// so an armour item with an unknown type is offered under all four, and the
// type name only narrows the offer.
constexpr u16 kAllArmourSlots = SlotBit(d3n::EVisualSlot::Torso) | SlotBit(d3n::EVisualSlot::Feet) |
                                SlotBit(d3n::EVisualSlot::Hands) | SlotBit(d3n::EVisualSlot::Legs);

// The type name with a `<Slot>_<Class>` suffix stripped: the class-suffixed
// armour spellings are one type per class, but the SLOT logic below is about
// the base word. Non-class suffixes (Runestone_A, CraftingPlan_Smith) pass
// through whole.
std::string_view BaseTypeName(std::string_view type) {
    const auto us = type.rfind('_');
    if (us == std::string_view::npos)
        return type;
    return d3n::playerClassFromTypeName(type).has_value() ? type.substr(0, us) : type;
}

// Where a type name narrows the armour offer to one body slot.
u16 ArmourSlotForType(std::string_view type) {
    if (type == "ChestArmor" || type == "Cloak" || type == "GenericChestArmor")
        return SlotBit(d3n::EVisualSlot::Torso);
    if (type == "Boots")
        return SlotBit(d3n::EVisualSlot::Feet);
    if (type == "Gloves")
        return SlotBit(d3n::EVisualSlot::Hands);
    if (type == "Legs")
        return SlotBit(d3n::EVisualSlot::Legs);
    return 0;
}

// Two-handers go in the right hand only; everything else held goes in either.
bool IsTwoHandedType(std::string_view type) {
    return type == "Sword2H" || type == "Axe2H" || type == "Mace2H" || type == "Flail2H" ||
           type == "MightyWeapon2H" || type == "Scythe2H" || type == "Bow" ||
           type == "Crossbow" || type == "Staff" || type == "CombatStaff" ||
           type == "Polearm" || type == "Spear" || type == "GenericBowWeapon" ||
           type == "GenericRangedWeapon";
}

} // namespace

std::string_view D3ItemRegistry::ItemTypeName(u32 gbidItemType) {
    const auto& m = TypeNamesByGbid();
    if (auto it = m.find(gbidItemType); it != m.end())
        return it->second;
    return {};
}

bool D3ItemRegistry::EnsureBuilt(IContentProvider* provider, ProgressMonitor* progress) {
    if (built_)
        return !items_.empty();
    // The budget is by measured cost, not step count: the Actor reads are the
    // build. An inert monitor makes every report a null check, so the
    // uninstrumented callers (tests, the headless CLI) pay nothing.
    ProgressMonitor inert;
    ProgressMonitor& m = progress ? *progress : inert;
    m.Begin("Building item registry", 19);
    bool tables;
    {
        ProgressMonitor step = m.Split(2);
        tables = LoadTables(provider, &step);
    }
    if (tables) {
        {
            ProgressMonitor step = m.Split(1);
            LoadStringLists(provider);
        }
        {
            ProgressMonitor step = m.Split(16);
            Classify(provider, &step);
        }
        BuildSets();
    }
    if (m.Cancelled()) {
        // Half a registry must not latch: classification stopped mid-read and
        // every untouched item would offer itself nowhere, forever.
        Clear();
        return false;
    }
    // Latch even an empty result — a storage with no GameBalance stays that
    // way, and the dressing room asks every frame it is open.
    built_ = true;
    return !items_.empty();
}

void D3ItemRegistry::Clear() {
    built_ = false;
    items_.clear();
    byGbid_.clear();
    sets_.clear();
    setByGbid_.clear();
    typeNames_.clear();
}

bool D3ItemRegistry::LoadTables(IContentProvider* provider, ProgressMonitor* progress) {
    auto adopt = [&](const d3n::GameBalance& gb) {
        for (const auto& src : gb.arItems) {
            if (src.szName.empty())
                continue;
            D3ItemRecord rec;
            rec.name = src.szName;
            rec.gbid = d3n::gbidHash(rec.name);
            rec.snoActor = src.snoActor.id;
            rec.gbidItemType = src.gbidItemType;
            rec.gbidSet = src.gbidSet;
            // First spelling wins on a gbid collision. The hash space is the
            // engine's own key space, so a real collision would be a data bug
            // the game shares; none exists in the shipped tables.
            if (byGbid_.emplace(rec.gbid, static_cast<u32>(items_.size())).second)
                items_.push_back(std::move(rec));
        }
    };

    // ---- The install: enumerate the GameBalance directory by name, read the
    // tables by their file id (the SNO id — D3's root is id-keyed).
    if (provider) {
        std::vector<std::string> listing = provider->ListFiles("Base/GameBalance", false);
        if (listing.empty())
            listing = provider->ListFiles("GameBalance", false);
        if (progress)
            progress->Begin("Reading item tables", listing.size());
        for (const std::string& path : listing) {
            if (progress) {
                progress->Note(path);
                progress->Worked();
            }
            if (!IsGam(path))
                continue;
            // A CASC install answers by file id; a plain content tree (the
            // corpus mounted as a provider) has no ids, so fall back to the
            // path read.
            const u32 fileId = provider->FileIdForPath(path);
            const auto bytes = fileId ? provider->ReadFile(ContentRef::FromFileId(fileId))
                                      : provider->ReadFile(ContentRef::FromPath(path));
            if (bytes) {
                if (auto gb = d3n::parseGameBalance(*bytes))
                    adopt(*gb);
            }
        }
        if (!items_.empty())
            return true;
    }

    // ---- The corpus snapshot: plain files, no ids needed.
    if (!fallbackDir_.empty()) {
        std::error_code ec;
        for (const auto& e : std::filesystem::directory_iterator(
                 fallbackDir_, std::filesystem::directory_options::skip_permission_denied, ec)) {
            if (!e.is_regular_file() || e.path().extension() != ".gam")
                continue;
            std::ifstream f(e.path(), std::ios::binary | std::ios::ate);
            if (!f)
                continue;
            std::vector<u8> bytes(static_cast<usize>(f.tellg()));
            f.seekg(0);
            f.read(reinterpret_cast<char*>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()));
            if (auto gb = d3n::parseGameBalance(bytes))
                adopt(*gb);
        }
    }
    return !items_.empty();
}

void D3ItemRegistry::LoadStringLists(IContentProvider* provider) {
    // One `.stl` through whichever door answers: the storage by locale path
    // (the live root spells it `enUS\StringList\Items.stl` and the name tree
    // holds only full paths, so the locales are probed), a path-keyed
    // provider, then the plain fallback directory. Everything here is
    // presentation data — a total miss leaves the registry exactly as it was.
    auto read = [&](const char* stem) -> std::optional<d3n::StringList> {
        if (provider) {
            static constexpr const char* kLocales[] = {
                "enUS", "enGB", "deDE", "esES", "esMX", "frFR", "itIT",
                "koKR", "plPL", "ptBR", "ptPT", "ruRU", "zhCN", "zhTW",
            };
            for (const char* loc : kLocales) {
                const std::string path =
                    std::string(loc) + "/StringList/" + stem + ".stl";
                if (const u32 id = provider->FileIdForPath(path)) {
                    if (auto bytes = provider->ReadFile(ContentRef::FromFileId(id)))
                        if (auto stl = d3n::parseStringList(*bytes))
                            return stl;
                } else if (auto bytes = provider->ReadFile(ContentRef::FromPath(path))) {
                    if (auto stl = d3n::parseStringList(*bytes))
                        return stl;
                }
            }
        }
        if (!fallbackStlDir_.empty()) {
            std::ifstream f(fallbackStlDir_ / (std::string(stem) + ".stl"),
                            std::ios::binary | std::ios::ate);
            if (f) {
                std::vector<u8> bytes(static_cast<usize>(f.tellg()));
                f.seekg(0);
                f.read(reinterpret_cast<char*>(bytes.data()),
                       static_cast<std::streamsize>(bytes.size()));
                if (auto stl = d3n::parseStringList(bytes))
                    return stl;
            }
        }
        return std::nullopt;
    };

    if (const auto types = read("ItemTypeNames")) {
        for (const auto& e : types->arEntries) {
            const auto key = d3n::stlText(e.szKey);
            if (!key.empty())
                typeNames_.emplace(d3n::gbidHash(key), std::string(key));
        }
    }
    if (const auto names = read("Items")) {
        // Joined on the gbid, not the spelling: that is the engine's own key
        // for both sides, and it is case-blind where a string compare is not.
        for (const auto& e : names->arEntries) {
            const auto key = d3n::stlText(e.szKey);
            if (const auto it = byGbid_.find(d3n::gbidHash(key)); it != byGbid_.end())
                items_[it->second].displayName = d3n::stlText(e.szValue);
        }
    }
    if (const auto sets = read("ItemSets")) {
        for (const auto& e : sets->arEntries) {
            const auto key = d3n::stlText(e.szKey);
            if (key.empty())
                continue;
            D3ItemSet set;
            set.key = key;
            set.displayName = d3n::stlText(e.szValue);
            set.gbid = d3n::gbidHash(key);
            if (setByGbid_.emplace(set.gbid, static_cast<u32>(sets_.size())).second)
                sets_.push_back(std::move(set));
        }
    }
}

void D3ItemRegistry::BuildSets() {
    // Membership. A record can name a set the STL does not (it cannot, in
    // the shipped data, but a modded storage might): such a set appears
    // key-less rather than losing its members.
    for (u32 i = 0; i < items_.size(); ++i) {
        const u32 g = items_[i].gbidSet;
        if (g == 0xFFFFFFFFu)
            continue;
        auto it = setByGbid_.find(g);
        if (it == setByGbid_.end()) {
            it = setByGbid_.emplace(g, static_cast<u32>(sets_.size())).first;
            sets_.push_back({.gbid = g});
        }
        sets_[it->second].members.push_back(i);
    }
    // Drop the STL keys nothing wears (ItemSets.stl ships 129 keys, the item
    // tables use 127) and sort what remains for the pickers.
    std::erase_if(sets_, [](const D3ItemSet& s) {
        return s.members.empty();
    });
    setByGbid_.clear();
    std::sort(sets_.begin(), sets_.end(), [](const D3ItemSet& a, const D3ItemSet& b) {
        const std::string& an = a.displayName.empty() ? a.key : a.displayName;
        const std::string& bn = b.displayName.empty() ? b.key : b.displayName;
        return an < bn;
    });
    for (u32 i = 0; i < sets_.size(); ++i)
        setByGbid_.emplace(sets_[i].gbid, i);

    // The set's class, then the members'. A single class-typed member classes
    // the whole set (the game sells them as one outfit); two members typed to
    // DIFFERENT classes would mean the derivation is wrong, so that reads as
    // neutral rather than guessing. The curated keys close the six sets whose
    // members are all generic-typed.
    for (auto& set : sets_) {
        u8 derived = kD3AllClasses;
        for (const u32 i : set.members) {
            const u8 m = items_[i].classMask;
            if (m == kD3AllClasses)
                continue;
            if (derived != kD3AllClasses && derived != m) {
                derived = kD3AllClasses;
                break;
            }
            derived = m;
        }
        if (derived == kD3AllClasses && !set.key.empty()) {
            if (const auto cls = d3n::playerClassForSetKey(set.key))
                derived = static_cast<u8>(1u << static_cast<u32>(*cls));
        }
        set.classMask = derived;
        if (derived == kD3AllClasses)
            continue;
        for (const u32 i : set.members)
            items_[i].classMask = derived;
    }

    for (auto& set : sets_) {
        std::sort(set.members.begin(), set.members.end(), [&](u32 a, u32 b) {
            return items_[a].name < items_[b].name;
        });
    }
}

const D3ItemSet* D3ItemRegistry::SetOf(const D3ItemRecord& rec) const {
    if (rec.gbidSet == 0xFFFFFFFFu)
        return nullptr;
    if (const auto it = setByGbid_.find(rec.gbidSet); it != setByGbid_.end())
        return &sets_[it->second];
    return nullptr;
}

std::string_view D3ItemRegistry::TypeNameOf(u32 gbidItemType) const {
    if (const auto it = typeNames_.find(gbidItemType); it != typeNames_.end())
        return it->second;
    return ItemTypeName(gbidItemType);
}

void D3ItemRegistry::Classify(IContentProvider* provider, ProgressMonitor* progress) {
    // The distinct Actors first: records share art (the art-test dupes are
    // six records on one sword), so the read list is deduplicated before any
    // IO, and each Actor is read and parsed exactly once.
    std::vector<i32> actorIds;
    {
        std::unordered_set<i32> seen;
        for (const auto& rec : items_) {
            if (rec.snoActor > 0 && seen.insert(rec.snoActor).second)
                actorIds.push_back(rec.snoActor);
        }
    }
    const std::unordered_map<i32, ActorTags> tags = ReadActorTags(provider, actorIds, progress);

    for (auto& rec : items_) {
        const std::string_view type = TypeNameOf(rec.gbidItemType);
        // The slot rules speak the BASE word: a ChestArmor_Wizard is chest
        // armour that happens to be class-locked, and letting the suffixed
        // spelling fall through as "unknown type" would drop the item from
        // the armour offer entirely now that the STL cracks it.
        const std::string_view base = BaseTypeName(type);
        const d3n::ItemTypeTraits traits = d3n::itemTypeTraits(rec.gbidItemType);

        // The class. Type first (the suffix or a class-locked base family);
        // sets refine this afterwards in BuildSets().
        if (const auto cls = d3n::playerClassFromTypeName(type))
            rec.classMask = static_cast<u8>(1u << static_cast<u32>(*cls));

        // The Actor's tags are the mechanism. A miss (no provider, or an id
        // nothing could serve) leaves the optionals empty and classification
        // falls back to the type name alone.
        if (const auto it = tags.find(rec.snoActor); it != tags.end()) {
            rec.lookValue = it->second.lookValue;
            rec.lookNameHash = it->second.lookNameHash;
            rec.hasHoldType = it->second.hasHoldType;
            rec.hasPerClassArt = it->second.hasPerClassArt;
            rec.hairStyle = it->second.hairStyle;
        }

        u16 mask = 0;
        // Armour: the Actor carries a look value. Attachment-family items
        // (helms, shoulders, off-hands, held weapons) often carry the tag
        // too — a helm ships 0x10400 = 0, meaningless because category 1 has
        // no name and never geoset-switches — so the armour offer needs the
        // type's word: a known armour type narrows it to one slot, a known
        // attachment type suppresses it, and only a type nothing cracked
        // falls back to all four.
        if (rec.lookValue.has_value()) {
            const u16 narrowed = ArmourSlotForType(base);
            const bool attachmentFamily = traits.helm || traits.shoulder || traits.offhandOnly ||
                                          rec.hairStyle != 0 || rec.hasHoldType;
            if (narrowed)
                mask |= narrowed;
            else if (!attachmentFamily && type.empty())
                mask |= kAllArmourSlots;
        }
        // Head: the helm flag, the hair tag only helms carry, or a suffixed
        // helm spelling the hash-keyed traits table cannot crack.
        if (traits.helm || rec.hairStyle != 0 || base == "Helm" || base == "SpiritStone")
            mask |= SlotBit(d3n::EVisualSlot::Head);
        if (traits.shoulder || base == "Shoulders")
            mask |= SlotBit(d3n::EVisualSlot::Shoulders);
        // Hands: off-hand-only types never go right; everything held goes
        // right, and one-handers go either side.
        if (traits.offhandOnly) {
            mask |= SlotBit(d3n::EVisualSlot::LeftHand);
        } else if (rec.hasHoldType && rec.snoActor > 0) {
            mask |= SlotBit(d3n::EVisualSlot::RightHand);
            if (!IsTwoHandedType(base))
                mask |= SlotBit(d3n::EVisualSlot::LeftHand);
        }
        rec.slotMask = mask;
    }
}

std::unordered_map<i32, D3ItemRegistry::ActorTags> D3ItemRegistry::ReadActorTags(
    IContentProvider* provider, const std::vector<i32>& ids, ProgressMonitor* progress) {
    std::unordered_map<i32, ActorTags> out;
    out.reserve(ids.size());
    if (progress)
        progress->Begin("Reading item art", ids.size());

    // ---- The provider, batched. A window of requests is kept in flight so
    // the provider's worker pool reads concurrently and one Pump delivers
    // many completions — the sequential ReadFile shape pays up to a frame of
    // delivery latency per file when this runs on a task thread. The window
    // bounds the byte buffers alive at once, not the throughput.
    //
    // The callback fires on the host's Pump thread and only moves the bytes
    // into its slot; the parse happens here, after Wait(). The shared_ptr
    // keeps a slot alive even when a cancelled request never fires it.
    std::vector<i32> missed;
    if (provider) {
        struct Slot {
            i32 sno;
            RequestId id;
            std::shared_ptr<RequestResult> res;
        };
        constexpr usize kWindow = 64;
        std::deque<Slot> inflight;
        auto settleFront = [&] {
            Slot s = std::move(inflight.front());
            inflight.pop_front();
            provider->Wait(s.id);
            if (s.res->ok) {
                if (const auto actor = d3n::parseActor(s.res->data))
                    out.emplace(s.sno, TagsOf(*actor));
            } else {
                missed.push_back(s.sno);
            }
            if (progress)
                progress->Worked();
        };
        for (const i32 sno : ids) {
            if (progress && progress->Cancelled())
                break;
            auto res = std::make_shared<RequestResult>();
            const RequestId id = provider->Request(
                ContentRef::FromFileId(static_cast<u32>(sno)),
                [res](RequestResult&& r) { *res = std::move(r); });
            if (id == kInvalidRequestId) {
                missed.push_back(sno);
                if (progress)
                    progress->Worked();
                continue;
            }
            inflight.push_back({sno, id, std::move(res)});
            while (inflight.size() >= kWindow)
                settleFront();
        }
        while (!inflight.empty())
            settleFront();
        if (progress && progress->Cancelled())
            return out;
    } else {
        missed = ids;
    }

    // ---- The fallback directory for what the provider could not answer.
    if (missed.empty() || fallbackActorDir_.empty())
        return out;
    if (!actorIndexBuilt_) {
        actorIndexBuilt_ = true;
        // One pass over the directory, reading each header's snoId (offset
        // 0x10) and nothing else. ~19k files, 20 bytes each — a second, once.
        std::error_code ec;
        for (const auto& e : std::filesystem::directory_iterator(
                 fallbackActorDir_, std::filesystem::directory_options::skip_permission_denied,
                 ec)) {
            if (!e.is_regular_file() || e.path().extension() != ".acr")
                continue;
            std::ifstream f(e.path(), std::ios::binary);
            u8 head[20];
            if (!f.read(reinterpret_cast<char*>(head), sizeof(head)))
                continue;
            u32 magic;
            i32 id;
            std::memcpy(&magic, head, 4);
            std::memcpy(&id, head + 16, 4);
            if (magic == 0xDEADBEEFu && id > 0)
                actorPathById_.emplace(id, e.path());
        }
    }
    for (const i32 sno : missed) {
        if (progress && progress->Cancelled())
            return out;
        const auto it = actorPathById_.find(sno);
        if (it == actorPathById_.end())
            continue;
        std::ifstream f(it->second, std::ios::binary | std::ios::ate);
        if (!f)
            continue;
        std::vector<u8> bytes(static_cast<usize>(f.tellg()));
        f.seekg(0);
        f.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (const auto actor = d3n::parseActor(bytes))
            out.emplace(sno, TagsOf(*actor));
        // Only counted when the provider never saw the id (no provider at
        // all); otherwise the bar already advanced when the miss settled.
        if (progress && !provider)
            progress->Worked();
    }
    return out;
}

D3ItemRegistry::ActorTags D3ItemRegistry::TagsOf(const d3n::Actor& actor) {
    ActorTags t;
    const d3n::ItemLook look = d3n::itemLook(actor);
    t.lookValue = look.lookValue;
    t.lookNameHash = look.lookName;
    t.hasHoldType = d3n::tagMapValue(actor.arTagMap, d3n::kTagItemHoldType).has_value();
    t.hasPerClassArt = d3n::tagMapValue(actor.arTagMap, d3n::kTagUsePerClassArt).value_or(0) != 0;
    t.hairStyle = d3n::tagMapValue(actor.arTagMap, d3n::kTagItemHairStyle).value_or(0);
    return t;
}

const D3ItemRecord* D3ItemRegistry::FindByName(std::string_view name) const {
    // The engine's own lookup semantics: names are addressed through the
    // case-insensitive hash, so this IS a case-insensitive find.
    return FindByGbid(d3n::gbidHash(name));
}

const D3ItemRecord* D3ItemRegistry::FindByGbid(u32 gbid) const {
    if (auto it = byGbid_.find(gbid); it != byGbid_.end())
        return &items_[it->second];
    return nullptr;
}

std::vector<u32> D3ItemRegistry::ItemsForSlot(d3n::EVisualSlot slot) const {
    std::vector<u32> out;
    for (u32 i = 0; i < items_.size(); ++i) {
        if (items_[i].CanGo(slot))
            out.push_back(i);
    }
    std::sort(out.begin(), out.end(), [&](u32 a, u32 b) {
        return items_[a].name < items_[b].name;
    });
    return out;
}

} // namespace whiteout::flakes::io
