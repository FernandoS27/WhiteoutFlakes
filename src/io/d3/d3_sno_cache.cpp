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
        return d3n::Group::Texture;
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
    lru_.clear();
    stats_.bytesResident = 0;
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

std::shared_ptr<const d3n::Actor> D3SnoCache::AdoptActor(i32 sno, std::span<const u8> bytes) {
    return Typed<d3n::Actor>(sno, d3n::Group::Actor, bytes);
}
std::shared_ptr<const d3n::Appearances> D3SnoCache::AdoptAppearance(i32 sno,
                                                                    std::span<const u8> bytes) {
    return Typed<d3n::Appearances>(sno, d3n::Group::Appearance, bytes);
}

d3n::Group D3SnoCache::GroupOf(i32 sno) {
    return Load(sno, {}).group;
}

} // namespace whiteout::flakes::io
