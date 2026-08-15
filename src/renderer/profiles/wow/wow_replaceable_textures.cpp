#include "renderer/profiles/wow/wow_replaceable_textures.h"

#include "io/m2/m2_model_adapter.h"
#include "whiteout/flakes/content_provider.h"

#include <algorithm>
#include <cctype>
#include <string_view>
#include <utility>
#include <vector>

namespace whiteout::flakes::renderer::profiles::wow {

namespace {

// CM2Model::ReplaceTexture(i + 11, tex), for i in [0, 3) — see
// CCharacterComponent::ReplaceMonsterSkin.
constexpr u32 kMonsterSkinFirstType = 11;
constexpr u32 kMonsterSkinSlots = 3;

bool IsMonsterSkin(u32 type) {
    return type >= kMonsterSkinFirstType && type < kMonsterSkinFirstType + kMonsterSkinSlots;
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
            v.texture[slots[i] - kMonsterSkinFirstType] = siblings[at + i];
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
    if (const u32 modelFile = ModelFileId(modelRef); modelFile != 0 && table_.Load(*provider_)) {
        for (const io::wow::MonsterSkin& skin : table_.ForModel(modelFile)) {
            SkinVariation v;
            v.label = "display " + std::to_string(skin.displayId);
            for (u32 slot = 0; slot < kMonsterSkinSlots; ++slot)
                if (skin.texture[slot] != 0)
                    v.texture[slot] = "#" + std::to_string(skin.texture[slot]);
            variations.push_back(std::move(v));
        }
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

    std::vector<std::string> byType(kMonsterSkinFirstType + kMonsterSkinSlots);
    for (u32 slot = 0; slot < kMonsterSkinSlots; ++slot)
        byType[kMonsterSkinFirstType + slot] = skin.texture[slot];
    adapter.SetReplaceableTextures(std::move(byType));

    // Slots filled, not variations named: one texture can serve several slots,
    // and a model can declare a type this skin leaves empty.
    return static_cast<usize>(
        std::count_if(model.textures.begin(), model.textures.end(), [&](const auto& t) {
            return IsMonsterSkin(t.type) && !skin.texture[t.type - kMonsterSkinFirstType].empty();
        }));
}

} // namespace whiteout::flakes::renderer::profiles::wow
