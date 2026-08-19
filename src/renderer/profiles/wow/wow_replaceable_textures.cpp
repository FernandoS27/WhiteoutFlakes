#include "renderer/profiles/wow/wow_replaceable_textures.h"

#include "io/m2/m2_model_adapter.h"
#include "whiteout/flakes/content_provider.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <string_view>
#include <utility>
#include <vector>

namespace whiteout::flakes::renderer::profiles::wow {

namespace {

using io::wow::kMonsterSkinSlots;
using io::wow::kMonsterSkinTypes;

// Which variation slot fills texture type @p type, or -1 for a type no display
// row touches. Not `type - 11`: the fourth slot fills type 5, so the set is not
// contiguous. See creature_skin_table.h for the measurement.
i32 MonsterSkinSlot(u32 type) {
    for (u32 slot = 0; slot < kMonsterSkinSlots; ++slot)
        if (kMonsterSkinTypes[slot] == type)
            return static_cast<i32>(slot);
    return -1;
}

bool IsMonsterSkin(u32 type) {
    return MonsterSkinSlot(type) >= 0;
}

/// The highest type any slot fills, so a by-type array can be sized once.
constexpr u32 kMaxMonsterSkinType = 13;

/// How many of the types @p slots names this variation actually fills.
usize Filled(const SkinVariation& v, const std::vector<u32>& slots) {
    usize n = 0;
    for (u32 type : slots) {
        const i32 slot = MonsterSkinSlot(type);
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
            const i32 slot = MonsterSkinSlot(slots[i]);
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
    // display id, so variation 0 is the one a creature of that kind usually
    // looks like rather than whichever row the table happened to hold first.
    //
    // One entry per *look*, not per display record. A model is named by every
    // display that uses it, and most of those differ in something the model
    // does not wear — scale, sound, blood level, a spell visual — so the
    // texture sets repeat: `cryptfiend` has 21 display rows behind 3 skins and
    // `cow` 10 behind 2. Offering all 21 is offering the same picture 19 times.
    // Keeping the first of each set keeps the lowest display id, which is the
    // ordering variation 0 already relies on.
    if (const u32 modelFile = ModelFileId(modelRef); modelFile != 0 && table_.Load(*provider_)) {
        std::vector<std::array<u32, kMonsterSkinSlots>> seen;
        for (const io::wow::MonsterSkin& skin : table_.ForModel(modelFile)) {
            std::array<u32, kMonsterSkinSlots> look{};
            for (u32 slot = 0; slot < kMonsterSkinSlots; ++slot)
                look[slot] = skin.texture[slot];
            if (std::find(seen.begin(), seen.end(), look) != seen.end())
                continue;
            seen.push_back(look);
            SkinVariation v;
            v.label = "display " + std::to_string(skin.displayId);
            for (u32 slot = 0; slot < kMonsterSkinSlots; ++slot)
                if (skin.texture[slot] != 0)
                    v.texture[slot] = "#" + std::to_string(skin.texture[slot]);
            variations.push_back(std::move(v));
        }
        // Display-id order decides which look is canonical, but not at the cost
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
    const auto slash = modelRef.path.find_last_of("/\\");
    const std::string dir =
        slash == std::string::npos ? std::string() : modelRef.path.substr(0, slash);
    const std::string_view stem = Stem(modelRef.path);

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
    // fills the first slot. A second slot stays white rather than wearing a
    // texture picked by guesswork — see GroupSiblings.
    for (std::string& sibling : siblings) {
        SkinVariation v;
        v.label = std::string(Stem(sibling));
        v.texture[0] = std::move(sibling);
        variations.push_back(std::move(v));
    }
    return variations;
}

usize WowReplaceableTextures::Apply(io::M2ModelAdapter& adapter, const ContentRef& modelRef) {
    const auto& model = adapter.SourceModel();

    // The cheap question first. A model with no monster-skin slot is the common
    // case, and answering it here is what keeps a session that never opens a
    // creature from reading eight megabytes of client database. The slots it
    // does declare are also what a skin has to fill, so collect them here.
    std::vector<u32> slots;
    for (const auto& tex : model.textures)
        if (IsMonsterSkin(tex.type) && std::find(slots.begin(), slots.end(), tex.type) == slots.end())
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

    std::vector<std::string> byType(kMaxMonsterSkinType + 1);
    for (u32 slot = 0; slot < kMonsterSkinSlots; ++slot)
        byType[kMonsterSkinTypes[slot]] = skin.texture[slot];
    adapter.SetReplaceableTextures(std::move(byType));

    // Slots filled, not variations named: one texture can serve several slots,
    // and a model can declare a type this skin leaves empty.
    return static_cast<usize>(
        std::count_if(model.textures.begin(), model.textures.end(), [&](const auto& t) {
            const i32 slot = MonsterSkinSlot(t.type);
            return slot >= 0 && !skin.texture[slot].empty();
        }));
}

} // namespace whiteout::flakes::renderer::profiles::wow
