#include "io/d3/d3_sno_cache.h"


#include <algorithm>
#include <cctype>
#include <cstring>
#include <optional>

namespace whiteout::flakes::io {

namespace {

constexpr u32 kSnoMagic = 0xDEADBEEFu;
constexpr usize kSnoHeaderSize = 16;

u32 ReadU32(std::span<const u8> b, usize at) {
    u32 v = 0;
    std::memcpy(&v, b.data() + at, sizeof(v));
    return v;
}

std::string ToLower(std::string_view s) {
    std::string out(s);
    for (char& c : out)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

} // namespace

bool LooksLikeD3(std::span<const u8> bytes) {
    return bytes.size() >= kSnoHeaderSize && ReadU32(bytes, 0) == kSnoMagic;
}

i32 D3SnoIdOfBytes(std::span<const u8> bytes) {
    if (bytes.size() < kSnoHeaderSize + 4 || !LooksLikeD3(bytes))
        return -1;
    return static_cast<i32>(ReadU32(bytes, kSnoHeaderSize));
}

d3n::Group D3GroupOfExtension(std::string_view ext) {
    const std::string e = ToLower(ext.empty() || ext.front() != '.' ? ext : ext.substr(1));
    if (e == "acr")
        return d3n::Group::Actor;
    if (e == "app")
        return d3n::Group::Appearance;
    if (e == "ani")
        return d3n::Group::Anim;
    if (e == "ans")
        return d3n::Group::AnimSet;
    if (e == "mat")
        return d3n::Group::Material;
    if (e == "tex")
        return d3n::Group::Textures;
    if (e == "prt")
        return d3n::Group::Particle;
    if (e == "clt")
        return d3n::Group::Cloth;
    if (e == "phy")
        return d3n::Group::Physics;
    if (e == "phm")
        return d3n::Group::PhysMesh;
    if (e == "ant")
        return d3n::Group::AnimTree;
    if (e == "shm")
        return d3n::Group::ShaderMap;
    if (e == "shd")
        return d3n::Group::Shaders;
    if (e == "efg")
        return d3n::Group::EffectGroup;
    return d3n::Group::Unknown;
}

d3n::Group D3GroupOfBytes(std::span<const u8> bytes) {
    if (!LooksLikeD3(bytes))
        return d3n::Group::Unknown;
    switch (ReadU32(bytes, 4)) {
    case 282:
        return d3n::Group::Actor;
    case 150:
        return d3n::Group::Shaders;
    case 260:
        return d3n::Group::Appearance;
    case 180:
        return d3n::Group::Particle;
    case 118:
        return d3n::Group::Anim;
    case 51:
        return d3n::Group::Cloth;
    case 47:
        return d3n::Group::EffectGroup;
    case 37:
        return d3n::Group::Physics;
    case 30:
        return d3n::Group::AnimTree;
    case 26:
        return d3n::Group::ShaderMap;
    case 25:
        return d3n::Group::Material;
    // 24 is both AnimSet and PhysMesh. Left unresolved rather than guessed:
    // every reference to either states its group in an AssetRef, so the only
    // caller that could land here is a host that typed a bare id.
    default:
        return d3n::Group::Unknown;
    }
}

d3n::Group D3GroupFor(const ContentRef& ref, std::span<const u8> bytes) {
    if (ref.IsPath()) {
        const auto dot = ref.path.find_last_of('.');
        if (dot != std::string::npos) {
            if (const auto g = D3GroupOfExtension(std::string_view(ref.path).substr(dot));
                g != d3n::Group::Unknown) {
                return g;
            }
        }
    }
    return D3GroupOfBytes(bytes);
}

// ---------------------------------------------------------------------------

void D3SnoCache::SetContentProvider(IContentProvider* provider) {
    if (provider_ == provider)
        return;
    provider_ = provider;
    Clear();
}

void D3SnoCache::SetBudgetBytes(usize bytes) {
    budget_ = bytes;
    EvictToBudget();
}

void D3SnoCache::Clear() {
    entries_.clear();
    names_.clear();
    atlases_.clear();
    lru_.clear();
    stats_.bytesResident = 0;
}

const std::string& D3SnoCache::NameOf(i32 sno) {
    static const std::string kNone;
    if (sno <= 0)
        return kNone;
    if (auto it = names_.find(sno); it != names_.end())
        return it->second;
    std::string name;
    if (provider_) {
        // `Base\Anim\Barbarian_Male_idle_01.ani` -> `Barbarian_Male_idle_01`.
        // A root with no CoreTOC spells the same entry `ani\123456`, which is
        // a number wearing a name; the digits-only check drops it so the
        // caller's own fallback wins rather than being shadowed by one.
        const std::string path = provider_->PathForFileId(static_cast<u32>(sno));
        const usize slash = path.find_last_of("/\\");
        const usize begin = (slash == std::string::npos) ? 0 : slash + 1;
        const usize dot = path.find_last_of('.');
        const usize end = (dot != std::string::npos && dot > begin) ? dot : path.size();
        std::string stem = path.substr(begin, end - begin);
        if (!stem.empty() &&
            stem.find_first_not_of("0123456789") != std::string::npos)
            name = std::move(stem);
    }
    // A miss is remembered too: it is answered by the same manifest that would
    // answer it next time, and the answer will be the same.
    return names_.emplace(sno, std::move(name)).first->second;
}

void D3SnoCache::Touch(Entry& e) {
    if (e.lru != lru_.end() && e.lru != lru_.begin())
        lru_.splice(lru_.begin(), lru_, e.lru);
}

void D3SnoCache::EvictToBudget() {
    // Back of the list is least recently used. A remembered miss weighs nothing
    // and is never worth evicting — it is the thing keeping a CASC probe from
    // happening again — so eviction only ever reclaims real entries.
    for (auto it = lru_.end(); stats_.bytesResident > budget_ && it != lru_.begin();) {
        --it;
        auto found = entries_.find(*it);
        if (found == entries_.end() || found->second.weight == 0)
            continue;
        stats_.bytesResident -= found->second.weight;
        ++stats_.evictions;
        entries_.erase(found);
        it = lru_.erase(it);
    }
}

D3SnoCache::Loaded D3SnoCache::Load(i32 sno, std::span<const u8> bytes) {
    if (sno <= 0)
        return {};

    if (auto it = entries_.find(sno); it != entries_.end()) {
        Touch(it->second);
        if (it->second.value)
            ++stats_.hits;
        else
            ++stats_.negativeHits;
        return {it->second.group, it->second.value};
    }

    std::optional<std::vector<u8>> read;
    if (bytes.empty()) {
        if (!provider_) {
            // No storage yet. Not a miss worth remembering — the same id will
            // resolve once the provider is configured.
            return {};
        }
        ++stats_.reads;
        read = provider_->ReadFile(ContentRef::FromFileId(static_cast<u32>(sno)));
        if (read && !read->empty())
            bytes = std::span<const u8>(read->data(), read->size());
    }

    Entry e;
    e.group = D3GroupOfBytes(bytes);
    e.weight = 0;
    if (e.group != d3n::Group::Unknown) {
        ++stats_.parses;
        switch (e.group) {
        case d3n::Group::Actor:
            if (auto v = d3n::parseActor(bytes))
                e.value = std::make_shared<const d3n::Actor>(std::move(*v));
            break;
        case d3n::Group::Appearance:
            if (auto v = d3n::parseAppearances(bytes))
                e.value = std::make_shared<const d3n::Appearances>(std::move(*v));
            break;
        case d3n::Group::Anim:
            if (auto v = d3n::parseAnim(bytes))
                e.value = std::make_shared<const d3n::Anim>(std::move(*v));
            break;
        case d3n::Group::Material:
            if (auto v = d3n::parseMaterial(bytes))
                e.value = std::make_shared<const d3n::Material>(std::move(*v));
            break;
        case d3n::Group::Physics:
            if (auto v = d3n::parsePhysics(bytes))
                e.value = std::make_shared<const d3n::Physics>(std::move(*v));
            break;
        case d3n::Group::Cloth:
            if (auto v = d3n::parseCloth(bytes))
                e.value = std::make_shared<const d3n::Cloth>(std::move(*v));
            break;
        case d3n::Group::Particle:
            if (auto v = d3n::parseParticle(bytes))
                e.value = std::make_shared<const d3n::Particle>(std::move(*v));
            break;
        case d3n::Group::EffectGroup:
            if (auto v = d3n::parseEffectGroup(bytes))
                e.value = std::make_shared<const d3n::EffectGroup>(std::move(*v));
            break;
        case d3n::Group::ShaderMap:
            if (auto v = d3n::parseShaderMap(bytes))
                e.value = std::make_shared<const d3n::ShaderMap>(std::move(*v));
            break;
        case d3n::Group::Shaders:
            if (auto v = d3n::parseShaders(bytes))
                e.value = std::make_shared<const d3n::Shaders>(std::move(*v));
            break;
        default:
            break;
        }
    }
    // The version word cannot tell AnimSet from PhysMesh (both 24), so an
    // AnimSet is parsed on the *caller's* claim rather than on the sniff. A
    // wrong claim costs a failed parse, not a wrong type: the group recorded is
    // the one that actually parsed.
    if (!e.value && !bytes.empty() && ReadU32(bytes, 4) == 24) {
        if (auto v = d3n::parseAnimSet(bytes)) {
            ++stats_.parses;
            e.group = d3n::Group::AnimSet;
            e.value = std::make_shared<const d3n::AnimSet>(std::move(*v));
        }
    }

    if (e.value) {
        e.weight = bytes.size();
        ++stats_.misses;
        stats_.bytesResident += e.weight;
    } else {
        e.group = d3n::Group::Unknown;
        ++stats_.negativeMisses;
    }

    Loaded out{e.group, e.value};
    lru_.push_front(sno);
    e.lru = lru_.begin();
    entries_.emplace(sno, std::move(e));
    EvictToBudget();
    return out;
}

template <typename T>
std::shared_ptr<const T> D3SnoCache::Typed(i32 sno, d3n::Group want, std::span<const u8> bytes) {
    const Loaded e = Load(sno, bytes);
    if (!e.value || e.group != want)
        return nullptr;
    return std::static_pointer_cast<const T>(e.value);
}

std::shared_ptr<const d3n::Actor> D3SnoCache::Actor(i32 sno) {
    return Typed<d3n::Actor>(sno, d3n::Group::Actor);
}
std::shared_ptr<const d3n::Appearances> D3SnoCache::Appearance(i32 sno) {
    return Typed<d3n::Appearances>(sno, d3n::Group::Appearance);
}
std::shared_ptr<const d3n::Anim> D3SnoCache::Anim(i32 sno) {
    return Typed<d3n::Anim>(sno, d3n::Group::Anim);
}
std::shared_ptr<const d3n::AnimSet> D3SnoCache::AnimSet(i32 sno) {
    return Typed<d3n::AnimSet>(sno, d3n::Group::AnimSet);
}
std::shared_ptr<const d3n::Material> D3SnoCache::Material(i32 sno) {
    return Typed<d3n::Material>(sno, d3n::Group::Material);
}
std::shared_ptr<const d3n::Physics> D3SnoCache::Physics(i32 sno) {
    return Typed<d3n::Physics>(sno, d3n::Group::Physics);
}
std::shared_ptr<const d3n::Particle> D3SnoCache::Particle(i32 sno) {
    return Typed<d3n::Particle>(sno, d3n::Group::Particle);
}
std::shared_ptr<const d3n::Cloth> D3SnoCache::Cloth(i32 sno) {
    return Typed<d3n::Cloth>(sno, d3n::Group::Cloth);
}
std::shared_ptr<const d3n::EffectGroup> D3SnoCache::EffectGroup(i32 sno) {
    return Typed<d3n::EffectGroup>(sno, d3n::Group::EffectGroup);
}

std::vector<u8> D3SnoCache::ReadBytes(i32 sno) {
    if (!provider_ || sno < 0)
        return {};
    ++stats_.reads;
    auto read = provider_->ReadFile(ContentRef::FromFileId(static_cast<u32>(sno)));
    if (!read)
        return {};
    return std::move(*read);
}
std::shared_ptr<const d3n::ShaderMap> D3SnoCache::ShaderMap(i32 sno) {
    return Typed<d3n::ShaderMap>(sno, d3n::Group::ShaderMap);
}
std::shared_ptr<const d3n::Shaders> D3SnoCache::Shaders(i32 sno) {
    return Typed<d3n::Shaders>(sno, d3n::Group::Shaders);
}

std::shared_ptr<const d3n::Actor> D3SnoCache::AdoptActor(i32 sno, std::span<const u8> bytes) {
    return Typed<d3n::Actor>(sno, d3n::Group::Actor, bytes);
}
std::shared_ptr<const d3n::Appearances> D3SnoCache::AdoptAppearance(i32 sno,
                                                                    std::span<const u8> bytes) {
    return Typed<d3n::Appearances>(sno, d3n::Group::Appearance, bytes);
}

namespace {

// The `.tex` header, in file offsets. Same numbers WhiteoutLib's TEX parser
// uses (`tex_internal.h`), read here directly rather than through it: the
// parser has no metadata-only entry point and this wants twenty floats, not a
// decoded mip chain. See D3TextureAtlas.
constexpr usize kTexDescOffset = 0x20;   ///< {format, width, height, depth, ...}
constexpr usize kTexAtlasOffset = 0x218; ///< {frameCount, tableOffset, tableSize, ...}
constexpr usize kTexFrameStride = 80;    ///< {u0, v0, u1, v1, char name[64]}

/// @brief Does the record at @p at read as a frame rectangle?
///
/// The test that separates a frame from the table's leading junk slot: every
/// shipped rect is a sub-rect of the sheet, so all four numbers are in [0,1]
/// and both extents are positive. What sits in slot 0 passes none of that —
/// `Axe_norm_unique_04`'s sheet holds the integers 4, 5, 6, 7 there (positive
/// denormals as floats, which a bare `u1 > u0` test would accept) and
/// `Wand_norm_unique_01`'s holds four zeros.
bool IsFrameRect(std::span<const u8> b, usize at) {
    f32 r[4];
    std::memcpy(r, b.data() + at, sizeof(r));
    for (f32 v : r) {
        if (!(v >= 0.0f && v <= 1.0f))
            return false;
    }
    return r[2] - r[0] > 1e-6f && r[3] - r[1] > 1e-6f;
}

} // namespace

std::shared_ptr<const D3TextureAtlas> D3SnoCache::TextureAtlas(i32 sno) {
    if (sno < 0)
        return nullptr;
    if (auto it = atlases_.find(sno); it != atlases_.end())
        return it->second;

    std::shared_ptr<const D3TextureAtlas> out;
    const std::vector<u8> bytes = ReadBytes(sno);
    const std::span<const u8> b{bytes};
    if (b.size() >= kTexAtlasOffset + 12 && ReadU32(b, 0) == kSnoMagic) {
        const u32 count = ReadU32(b, kTexAtlasOffset);
        const u32 tableAt = ReadU32(b, kTexAtlasOffset + 4);
        const u32 tableBytes = ReadU32(b, kTexAtlasOffset + 8);
        // The frame table does not start at `frameTableOffset`: every sheet
        // declares `count * 80` bytes there and puts ONE record before the
        // frames, so the real frames are `count` records starting one slot in.
        // Taking that slot for a frame loses the sheet's LAST tile, and it
        // cannot be one anyway — the engine reads the tile size off frame 0 and
        // this record is not a rectangle (integers on one sheet, zeros on
        // another).
        //
        // Measured over every sheet the corpus's 18,473 particle materials
        // reach: the skip is **1 on all 34,870 of them**, so this is a fixed
        // leading slot and not junk to be searched for. One test rather than a
        // search, because a search that finds three junk-looking records in a
        // row would silently eat three tiles, and one that finds none must not
        // shift the table either. With the skip in place no one-frame sheet
        // fails to cover its texture and only 8 multi-frame sheets do not tile.
        const u32 first =
            (static_cast<u64>(tableAt) + kTexFrameStride <= b.size() && !IsFrameRect(b, tableAt))
                ? 1u
                : 0u;
        const u64 end =
            static_cast<u64>(tableAt) + static_cast<u64>(first + count) * kTexFrameStride;
        (void)tableBytes;
        if (count > 0 && tableAt > 0 && end <= b.size()) {
            auto a = std::make_shared<D3TextureAtlas>();
            a->width = ReadU32(b, kTexDescOffset + 4);
            a->height = ReadU32(b, kTexDescOffset + 8);
            a->leadSkip = first;
            a->frames.reserve(count);
            for (u32 i = 0; i < count; ++i) {
                const usize at = tableAt + static_cast<usize>(first + i) * kTexFrameStride;
                f32 r[4];
                std::memcpy(r, b.data() + at, sizeof(r));
                a->frames.push_back({r[0], r[1], r[2], r[3]});
            }
            out = std::move(a);
        }
    }
    atlases_.emplace(sno, out);
    return out;
}

d3n::Group D3SnoCache::GroupOf(i32 sno) {
    return Load(sno, {}).group;
}

std::shared_ptr<const d3n::Shaders> D3ResolveShaders(const d3n::UberMaterial& material,
                                                     D3SnoCache* cache) {
    if (!cache || !material.snoShaderMap.valid())
        return nullptr;
    const auto map = cache->ShaderMap(material.snoShaderMap.id);
    if (!map)
        return nullptr;
    for (const u32 tag : kD3OpaqueTagChain) {
        for (const auto& e : map->arShaders) {
            if (e.dwTagId == tag && e.snoShader.valid())
                return cache->Shaders(e.snoShader.id);
        }
    }
    // Nothing on the chain. Shipped maps are small and single-tagged often
    // enough that refusing here would drop real state, so the first valid entry
    // stands in — a more generic program is exactly what the fall-through
    // produces anyway.
    for (const auto& e : map->arShaders) {
        if (e.snoShader.valid())
            return cache->Shaders(e.snoShader.id);
    }
    return nullptr;
}

u32 D3ScenePassIndex(const d3n::Shaders& shaders) {
    for (usize i = 0; i < shaders.arRenderPasses.size(); ++i) {
        if (shaders.arRenderPasses[i].dwUnknown00 != kD3RenderPhaseDistortion)
            return static_cast<u32>(i);
    }
    return 0;
}

i32 D3DistortionPassIndex(const d3n::Shaders& shaders) {
    for (usize i = 0; i < shaders.arRenderPasses.size(); ++i) {
        if (shaders.arRenderPasses[i].dwUnknown00 == kD3RenderPhaseDistortion)
            return static_cast<i32>(i);
    }
    return -1;
}

u32 D3ChainStageTypes(const d3n::UberMaterial& material, D3SnoCache* cache,
                      std::array<i32, kD3MaxChainStages>& out, bool distortionPass) {
    out.fill(0);
    const auto shaders = D3ResolveShaders(material, cache);
    if (!shaders || shaders->arRenderPasses.empty())
        return 0;
    i32 index = static_cast<i32>(D3ScenePassIndex(*shaders));
    if (distortionPass) {
        index = D3DistortionPassIndex(*shaders);
        if (index < 0)
            return 0;
    }
    const auto& pass0 = shaders->arRenderPasses[static_cast<usize>(index)];
    // TWO chain shapes, and they do not index the same way.
    //
    // `Legacy.fx :: ps_legacy` is the combine block's chain, and `ps_legacy`
    // alone: the family's other 27 pixel entry points are a program each and
    // none of them is the fixed-function chain. Its stage i is the i-th
    // CONTENT stage — declaration order with the scene-depth stage dropped,
    // which is how the block itself counts.
    //
    // `Distortion.fx :: ps_distortion2tex` reads no block at all, so the
    // surface table synthesises replace-then-add straight down
    // `arTextureStages` and its stage i is simply the i-th declared stage.
    // Leaving it out of this function does not lose a chain — it loses the
    // ANIMATION on one, silently: the palette entry a scrolling stage names is
    // written here and nowhere else, and 22 of the 55 shipped two-tex stages
    // animate. They stood still.
    const bool legacyChain =
        pass0.szEffectFile == "Legacy.fx" && pass0.szPixelShaderEntry == "ps_legacy";
    const bool twoTex = pass0.szPixelShaderEntry == "ps_distortion2tex";
    if (!legacyChain && !twoTex)
        return 0;
    u32 n = 0;
    for (const auto& stage : pass0.arTextureStages) {
        if (legacyChain && stage.dwTextureType == kD3TextureTypeSceneDepth)
            continue;
        if (n >= kD3MaxChainStages)
            break;
        out[n++] = stage.dwTextureType;
    }
    return n;
}

} // namespace whiteout::flakes::io
