#include "storage_paths.h"

#include <array>
#include <cctype>
#include <filesystem>

namespace whiteout::flakes::io {

namespace {

namespace fs = std::filesystem;

constexpr std::array<const char*, 5> kTextureExts = {".blp", ".dds", ".tga", ".png", ".tif"};
constexpr std::array<const char*, 2> kModelExts = {".mdx", ".mdl"};

// Warcraft III's four TVFS mod roots, spelled once.
constexpr std::string_view kWc3Root = "war3.w3mod:";
constexpr const char* kSd = "war3.w3mod:";
constexpr const char* kHd = "war3.w3mod:_hd.w3mod:";
constexpr const char* kDe = "war3.w3mod:_de.w3mod:";
constexpr const char* kDeprecated = "war3.w3mod:_deprecated.w3mod:";
// A path the storage stores with no chain at all. 3.0.0 dropped the
// chain-less duplicate spelling that Reforged listed most of its content
// under, so this reaches almost nothing now — but "almost" is why it stays,
// and it is last because it is the one spelling that cannot say which tier
// answered.
constexpr const char* kBare = "";

// `_de` comes after every overlay the older tiers read before 3.0.0, so it
// only answers what they miss: the Definitive cinematics sit under the root
// but draw with textures that exist only under `_de`.
constexpr std::array<const char*, 5> kChainClassic = {kSd, kHd, kDeprecated, kDe, kBare};
constexpr std::array<const char*, 5> kChainReforged = {kHd, kSd, kDeprecated, kDe, kBare};
constexpr std::array<const char*, 5> kChainDefinitive = {kDe, kHd, kSd, kDeprecated, kBare};

// The overlay segment each tier is named by, longest first so `_deprecated`
// cannot be mistaken for a prefix of something else.
struct TierTag {
    std::string_view segment;
    Wc3ArtTier tier;
};
constexpr std::array<TierTag, 2> kTierTags = {{
    {"_de.w3mod:", Wc3ArtTier::Definitive},
    {"_hd.w3mod:", Wc3ArtTier::Reforged},
}};

bool HasExtension(const std::string& ext, const char* const* list, usize count) {
    for (usize i = 0; i < count; ++i)
        if (ext == list[i])
            return true;
    return false;
}

char Lower(char c) {
    return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
}

} // namespace

std::string NormalizeCascPath(std::string_view relPath) {
    std::string out;
    out.reserve(relPath.size());
    for (char c : relPath)
        out += Lower(c == '/' ? '\\' : c);
    usize start = 0;
    while (start < out.size() && (out[start] == '\\' || out[start] == '/'))
        ++start;
    if (start > 0)
        out.erase(0, start);
    return out;
}

std::string ToListingPath(std::string_view stored) {
    const auto colon = stored.rfind(':');
    if (colon != std::string_view::npos)
        stored.remove_prefix(colon + 1);
    std::string out;
    out.reserve(stored.size());
    for (char c : stored)
        out += Lower(c == '\\' ? '/' : c);
    usize start = 0;
    while (start < out.size() && out[start] == '/')
        ++start;
    if (start > 0)
        out.erase(0, start);
    return out;
}

std::span<const char* const> Wc3ModChain(Wc3ArtTier tier) {
    switch (tier) {
    case Wc3ArtTier::Definitive:
        return kChainDefinitive;
    case Wc3ArtTier::Reforged:
        return kChainReforged;
    default:
        return kChainClassic;
    }
}

bool HasWc3ModChain(std::string_view stored) {
    if (stored.size() <= kWc3Root.size())
        return false;
    for (usize i = 0; i < kWc3Root.size(); ++i)
        if (Lower(stored[i]) != kWc3Root[i])
            return false;
    return true;
}

std::string_view StripWc3ModRoot(std::string_view stored) {
    return HasWc3ModChain(stored) ? stored.substr(kWc3Root.size()) : stored;
}

std::optional<Wc3ArtTier> Wc3TierOfPath(std::string_view stored) {
    if (!HasWc3ModChain(stored))
        return std::nullopt;
    // Only the segment directly under the root decides the tier. Looking
    // anywhere in the string would let a locale or tileset sub-mod of one tier
    // — "war3.w3mod:_de.w3mod:_tilesets/a.w3mod:" — be read as a
    // different one, and would answer for a map that merely happens to have
    // "_hd.w3mod" in a folder name.
    std::string_view rest = stored.substr(kWc3Root.size());
    for (const TierTag& tag : kTierTags) {
        if (rest.size() < tag.segment.size())
            continue;
        bool match = true;
        for (usize i = 0; i < tag.segment.size() && match; ++i)
            match = Lower(rest[i]) == tag.segment[i];
        if (match)
            return tag.tier;
    }
    // Under the root but under no overlay: the classic art.
    return Wc3ArtTier::Classic;
}

Wc3ArtTier Wc3TierForModel(std::string_view stored, bool hasHdMaterial) {
    if (!hasHdMaterial)
        return Wc3ArtTier::Classic;
    return Wc3TierOfPath(stored) == Wc3ArtTier::Definitive ? Wc3ArtTier::Definitive
                                                          : Wc3ArtTier::Reforged;
}

std::string NormalizeListingDir(const std::string& directory) {
    std::string dir = ToListingPath(directory);
    while (!dir.empty() && dir.back() == '/')
        dir.pop_back();
    return dir;
}

bool MatchesListingDir(const std::string& relPath, const std::string& dir, bool recursive) {
    usize offset = 0;
    if (!dir.empty()) {
        if (relPath.size() <= dir.size() + 1 || relPath.compare(0, dir.size(), dir) != 0 ||
            relPath[dir.size()] != '/')
            return false;
        offset = dir.size() + 1;
    }
    return recursive || relPath.find('/', offset) == std::string::npos;
}

std::string StripExtension(std::string_view path) {
    const auto dot = path.rfind('.');
    return std::string(dot != std::string_view::npos ? path.substr(0, dot) : path);
}

std::string GetLowerExtension(std::string_view relPath) {
    std::string ext = fs::path(relPath).extension().string();
    for (auto& c : ext)
        c = Lower(c);
    return ext;
}

std::pair<const char* const*, usize> AltExtensionsFor(const std::string& ext) {
    if (HasExtension(ext, kTextureExts.data(), kTextureExts.size()))
        return {kTextureExts.data(), kTextureExts.size()};
    if (HasExtension(ext, kModelExts.data(), kModelExts.size()))
        return {kModelExts.data(), kModelExts.size()};
    return {nullptr, 0};
}

AssetKind ClassifyByExtension(const std::string& ext) {
    if (HasExtension(ext, kTextureExts.data(), kTextureExts.size()))
        return AssetKind::Texture;
    if (HasExtension(ext, kModelExts.data(), kModelExts.size()))
        return AssetKind::Model;
    return AssetKind::Other;
}

} // namespace whiteout::flakes::io
