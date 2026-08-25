#include "renderer/profiles/wow/wow_replaceable_textures.h"

#include "io/m2/m2_model_adapter.h"
#include "whiteout/flakes/content_provider.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace whiteout::flakes::renderer::profiles::wow {

namespace {

using io::wow::kMaxReplaceableType;
using io::wow::kMonsterSkinSlots;
using io::wow::kReplaceableSlots;
using io::wow::kReplaceableTypes;
using io::wow::ReplaceableSlotOfType;

/// Which family of table can fill texture type @p type: the creature one holds
/// the first four slots, the item one the rest.
///
/// Slot 3 (type 5) is in both tables and counts as a creature's here. It is the
/// creature array's fourth entry and only 72 of the item table's 141309 rows,
/// so a model that declares it and nothing else is a creature.
bool IsMonsterSkin(u32 type) {
    const i32 slot = ReplaceableSlotOfType(type);
    return slot >= 0 && static_cast<u32>(slot) < kMonsterSkinSlots;
}

bool IsItemSkin(u32 type) {
    const i32 slot = ReplaceableSlotOfType(type);
    return slot >= 0 && static_cast<u32>(slot) >= kMonsterSkinSlots;
}

/// One picker entry per distinct set of files, not per record.
///
/// A model is named by every record that uses it, and most of those differ in
/// something the model does not wear — scale, sound, blood level or a
/// spell visual for a creature; a whole separate item for an appearance — so
/// the texture sets repeat. `cryptfiend` has 21 display rows behind 3 skins,
/// `cow` 10 behind 2, `shield_1h_artifactmagnar_d_03` 8 behind 4. Offering all
/// of them is offering the same picture over and over; keeping the first of
/// each set keeps the lowest id, which is the ordering variation 0 already
/// relies on.
///
/// Written once for both tables because both hand back the same shape: an id
/// and one fileDataID per slot, in `kReplaceableTypes` order. A creature row is
/// only as wide as its own four slots, hence `std::size`.
template <typename Row>
std::vector<SkinVariation> DistinctLooks(std::span<const Row> rows) {
    std::vector<SkinVariation> out;
    std::vector<std::array<u32, kReplaceableSlots>> seen;
    for (const Row& row : rows) {
        std::array<u32, kReplaceableSlots> look{};
        for (usize slot = 0; slot < std::size(row.texture); ++slot)
            look[slot] = row.texture[slot];
        if (std::find(seen.begin(), seen.end(), look) != seen.end())
            continue;
        seen.push_back(look);
        SkinVariation v;
        v.label = "display " + std::to_string(row.displayId);
        for (u32 slot = 0; slot < kReplaceableSlots; ++slot)
            if (look[slot] != 0)
                v.texture[slot] = "#" + std::to_string(look[slot]);
        out.push_back(std::move(v));
    }
    return out;
}

/// How many of the types @p slots names this variation actually fills.
usize Filled(const SkinVariation& v, const std::vector<u32>& slots) {
    usize n = 0;
    for (u32 type : slots) {
        const i32 slot = ReplaceableSlotOfType(type);
        if (slot >= 0 && !v.texture[slot].empty())
            ++n;
    }
    return n;
}

std::string_view Stem(std::string_view path) {
    const auto slash = path.find_last_of("/\\");
    if (slash != std::string_view::npos)
        path.remove_prefix(slash + 1);
    const auto dot = path.find_last_of('.');
    return dot == std::string_view::npos ? path : path.substr(0, dot);
}

// Case-insensitive, because half these names come from a listing the provider
// lowercased and half from whatever the host's open dialog handed back.
bool IEqual(std::string_view a, std::string_view b) {
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::tolower(static_cast<unsigned char>(x)) ==
                      std::tolower(static_cast<unsigned char>(y));
           });
}

bool IEndsWith(std::string_view s, std::string_view tail) {
    return s.size() >= tail.size() && IEqual(s.substr(s.size() - tail.size()), tail);
}

/// True when @p path's only distinguishing part is a number that *is* its own
/// fileDataID — `revenantair_4067960.blp`, `sporebat3mount_6254095.blp`. Those
/// come from a listfile that could not name the file, so the name carries no
/// information about what the texture is for.
bool IsListfilePlaceholder(const std::string& path, std::string_view stem,
                           const io::IContentProvider& provider) {
    const std::string_view s = Stem(path);
    const auto underscore = s.find_last_of('_');
    if (underscore == std::string_view::npos || underscore < stem.size())
        return false;
    const std::string_view digits = s.substr(underscore + 1);
    if (digits.empty() ||
        !std::all_of(digits.begin(), digits.end(), [](char c) { return c >= '0' && c <= '9'; }))
        return false;
    u64 id = 0;
    for (char c : digits) {
        id = id * 10 + static_cast<u64>(c - '0');
        if (id > 0xFFFFFFFFull)
            return false;
    }
    return static_cast<u32>(id) == provider.FileIdForPath(path);
}

bool IStartsWith(std::string_view s, std::string_view head) {
    return s.size() >= head.size() && IEqual(s.substr(0, head.size()), head);
}

/// The path the storage knows @p path by, or empty when it knows none.
///
/// A host opens a model by whatever its file dialog produced, which for a file
/// on disk is absolute — and beside an extracted `.m2` there is often nothing
/// else at all, no `.blp` to find, while the install's own folder holds every
/// skin the model can wear. ListFiles answers an absolute directory from disk
/// alone (no archive entry carries a drive letter), so reaching that folder
/// means naming it the way the storage does.
///
/// A storage matches a path by its longest recognised suffix, so every suffix
/// longer than its own name for the file answers with the same id; the
/// *shortest* one that still does is that name.
std::string GameRelative(const std::string& path, const io::IContentProvider& provider) {
    std::string norm = path;
    std::replace(norm.begin(), norm.end(), '\\', '/');
    const u32 id = provider.FileIdForPath(norm);
    if (id == 0)
        return {};
    std::string known;
    for (usize at = 0; at != std::string::npos;) {
        if (std::string tail = norm.substr(at); provider.FileIdForPath(tail) == id)
            known = std::move(tail);
        const auto slash = norm.find('/', at);
        at = slash == std::string::npos ? std::string::npos : slash + 1;
    }
    return known;
}

/// Pair a model's `.blp` siblings into skins, for the models that declare more
/// than one slot. Empty when the names do not say how, which is the honest
/// answer more often than not: `CreatureDisplayInfo` is the only place the
/// pairing actually lives, and it is not recoverable from the folder. `batpet`
/// is the proof — the client pairs `batpet` with `batpet*glow*` and `batpetfire`
/// with `batpetglowfire`, which no ordering of those six names produces.
///
/// What does work is the `<variant>_<part>` spelling, where every variant
/// carries the same set of trailing tokens: `crabmount_body` / `crabmount_saddle`
/// against `crabmount_blue_body` / `crabmount_blue_saddle`. Requiring *every*
/// group to carry the *same* tokens is what keeps this from firing on a folder
/// that merely happens to contain underscores.
std::vector<SkinVariation> GroupSiblings(const std::vector<std::string>& siblings,
                                         std::string_view stem, const std::vector<u32>& slots) {
    if (slots.size() < 2 || siblings.size() < slots.size())
        return {};

    // Split each name into the variant it belongs to and the part it fills.
    std::vector<std::pair<std::string, std::string>> split; // (variant, part)
    for (const std::string& s : siblings) {
        const std::string_view rest = Stem(s).substr(stem.size());
        const auto sep = rest.find_last_of('_');
        if (sep == std::string_view::npos || sep + 1 == rest.size())
            return {};
        split.emplace_back(std::string(rest.substr(0, sep)), std::string(rest.substr(sep + 1)));
    }

    // Every variant must fill exactly the same parts, and there must be one
    // part per slot: anything else and the pairing is a guess.
    std::vector<std::string> parts;
    for (const auto& [variant, part] : split)
        if (variant == split.front().first)
            parts.push_back(part);
    if (parts.size() != slots.size() || split.size() % parts.size() != 0)
        return {};

    std::vector<SkinVariation> out;
    for (usize at = 0; at < split.size(); at += parts.size()) {
        SkinVariation v;
        v.label = std::string(stem) + split[at].first;
        for (usize i = 0; i < parts.size(); ++i) {
            if (split[at + i].first != split[at].first || split[at + i].second != parts[i])
                return {}; // a variant with a different part set — not this shape
            const i32 slot = ReplaceableSlotOfType(slots[i]);
            if (slot < 0)
                return {};
            v.texture[slot] = siblings[at + i];
        }
        out.push_back(std::move(v));
    }
    return out;
}

} // namespace

void WowReplaceableTextures::SetContentProvider(io::IContentProvider* provider) {
    if (provider_ == provider)
        return;
    provider_ = provider;
    Clear();
}

u32 WowReplaceableTextures::ModelFileId(const ContentRef& ref) const {
    if (ref.IsFileId())
        return ref.fileId;
    if (!provider_ || ref.path.empty())
        return 0;
    return provider_->FileIdForPath(ref.path);
}

const std::vector<SkinVariation>&
WowReplaceableTextures::Variations(const ContentRef& modelRef) const {
    static const std::vector<SkinVariation> kNone;
    const auto it = byModel_.find(modelRef.Describe());
    return it == byModel_.end() ? kNone : it->second;
}

std::vector<SkinVariation> WowReplaceableTextures::FindVariations(const ContentRef& modelRef,
                                                                  const std::vector<u32>& slots) {
    std::vector<SkinVariation> variations;

    // The client's own answer, when the storage can name the model. Ordered by
    // record id, so variation 0 is the look the model usually has rather than
    // whichever row the table happened to hold first.
    //
    // Whichever family the model's slots belong to, and only that one: the two
    // sets of tables are half a million rows between them, and a shield has no
    // business parsing CreatureDisplayInfo. The item table is still tried when
    // the creature one came back empty, so a model the wrong guess would leave
    // white costs a lookup instead of a slot.
    if (const u32 modelFile = ModelFileId(modelRef); modelFile != 0) {
        const bool creature = std::any_of(slots.begin(), slots.end(), IsMonsterSkin);
        const bool item = std::any_of(slots.begin(), slots.end(), IsItemSkin);
        if (creature && table_.Load(*provider_))
            variations = DistinctLooks<io::wow::MonsterSkin>(table_.ForModel(modelFile));
        if (variations.empty() && item && items_.Load(*provider_))
            variations = DistinctLooks<io::wow::ItemAppearance>(items_.ForModel(modelFile));
        // Record-id order decides which look is canonical, but not at the cost
        // of a white patch: a row may leave a slot the model *declares* empty
        // while a later row fills it. `necromancer2` is the case — 106 displays,
        // the lowest-id 85 of them naming only a body, the other 21 naming the
        // cloak the model's type-12 texture is for. Stable, so among rows that
        // fill the same number of the model's slots the lowest id still wins.
        std::stable_sort(variations.begin(), variations.end(),
                         [&](const SkinVariation& a, const SkinVariation& b) {
                             return Filled(a, slots) > Filled(b, slots);
                         });
        if (!variations.empty())
            return variations;
    }

    // Otherwise the model's own folder. ReplaceMonsterSkin appends the
    // variation name to the model's directory, so every skin this model can
    // wear is a `.blp` beside it — and named after it, which is what separates
    // the skins from the effect and reflection textures creatures share.
    if (modelRef.IsFileId() || modelRef.path.empty())
        return variations;
    // The storage's folder when it can place the model, the host's otherwise:
    // an extracted `.m2` sitting alone on disk wears the same skins as the one
    // in the install, and only one of the two folders holds them.
    const std::string known = GameRelative(modelRef.path, *provider_);
    const std::string& path = known.empty() ? modelRef.path : known;
    const auto slash = path.find_last_of("/\\");
    const std::string dir = slash == std::string::npos ? std::string() : path.substr(0, slash);
    const std::string_view stem = Stem(path);

    std::vector<std::string> siblings;
    for (std::string& sibling : provider_->ListFiles(dir, /*recursive*/ false)) {
        if (IEndsWith(sibling, ".blp") && IStartsWith(Stem(sibling), stem))
            siblings.push_back(std::move(sibling));
    }
    std::sort(siblings.begin(), siblings.end());
    // A name that is only the model's stem and the file's own id is not a name:
    // the community listfile spells an entry it cannot identify that way, and
    // `_` sorts ahead of every letter, so such a file becomes variation 0 and
    // the real skins move down. `revenantair` is the case — five named skins
    // (black, blue, green, light, rust) losing to `revenantair_4067960.blp`.
    // Kept in the list, because it is still a texture beside the model and the
    // host may want it; just never the default.
    std::stable_partition(siblings.begin(), siblings.end(), [&](const std::string& s) {
        return !IsListfilePlaceholder(s, stem, *provider_);
    });

    if (auto grouped = GroupSiblings(siblings, stem, slots); !grouped.empty())
        return grouped;

    // Nothing said how to pair them, so each file is a skin on its own and
    // fills the first slot the model declares — type 11 for a creature, type
    // 2 for an item, which is what a shield's four coloured siblings are for. A
    // second slot stays white rather than wearing a texture picked by
    // guesswork — see GroupSiblings.
    u32 first = kReplaceableSlots - 1;
    for (u32 type : slots)
        first = std::min(first, static_cast<u32>(ReplaceableSlotOfType(type)));
    for (std::string& sibling : siblings) {
        SkinVariation v;
        v.label = std::string(Stem(sibling));
        v.texture[first] = std::move(sibling);
        variations.push_back(std::move(v));
    }
    return variations;
}

usize WowReplaceableTextures::Apply(io::M2ModelAdapter& adapter, const ContentRef& modelRef) {
    const auto& model = adapter.SourceModel();

    // The cheap question first. A model with no replaceable slot at all is the
    // common case, and answering it here is what keeps a session that opens
    // neither a creature nor an item from reading a client database. The slots
    // it does declare are also what a look has to fill, so collect them here.
    std::vector<u32> slots;
    for (const auto& tex : model.textures)
        if (ReplaceableSlotOfType(tex.type) >= 0 &&
            std::find(slots.begin(), slots.end(), tex.type) == slots.end())
            slots.push_back(tex.type);
    if (!provider_ || slots.empty())
        return 0;
    std::sort(slots.begin(), slots.end());

    // Re-found on every spawn rather than cached-and-reused: the entry exists so
    // a host can ask later, not to make the second spawn cheaper.
    auto& variations = byModel_[modelRef.Describe()];
    variations = FindVariations(modelRef, slots);
    if (variations.empty())
        return 0;
    const SkinVariation& skin = variations[variation_ % variations.size()];

    std::vector<std::string> byType(kMaxReplaceableType + 1);
    for (u32 slot = 0; slot < kReplaceableSlots; ++slot)
        byType[kReplaceableTypes[slot]] = skin.texture[slot];
    adapter.SetReplaceableTextures(std::move(byType));

    // Slots filled, not variations named: one texture can serve several slots,
    // and a model can declare a type this skin leaves empty.
    return static_cast<usize>(
        std::count_if(model.textures.begin(), model.textures.end(), [&](const auto& t) {
            const i32 slot = ReplaceableSlotOfType(t.type);
            return slot >= 0 && !skin.texture[slot].empty();
        }));
}

} // namespace whiteout::flakes::renderer::profiles::wow
