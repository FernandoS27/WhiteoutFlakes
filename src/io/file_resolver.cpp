#include "file_resolver.h"
#include "whiteout/flakes/types.h"
#include "whiteout/flakes/util/path_utf8.h"

#include <cctype>
#include <optional>
#include <vector>

namespace whiteout::flakes::io {
namespace fs = std::filesystem;

static constexpr const char* kTextureExts[] = {".blp", ".dds", ".tga", ".png", ".tif"};
static constexpr const char* kModelExts[] = {".mdx", ".mdl"};

namespace {

// ASCII-only, matching AssetManager::Normalize: the paths asked for are
// already lowercased that way.
std::string LowerAscii(std::string s) {
    for (auto& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// The entry of `listing` whose lowercased name is one of `wanted`, earliest
// in `wanted` first; among same-name candidates, the smallest name by bytes so
// the pick does not depend on directory order.
template <typename Listing>
std::optional<std::string> MatchEntry(const Listing& listing, std::span<const std::string> wanted,
                                      bool wantDirectory) {
    std::vector<const std::string*> best(wanted.size(), nullptr);
    for (const auto& entry : listing) {
        if (entry.isDirectory != wantDirectory)
            continue;
        for (usize i = 0; i < wanted.size(); ++i)
            if (entry.lower == wanted[i] && (!best[i] || entry.name < *best[i]))
                best[i] = &entry.name;
    }
    for (const std::string* b : best)
        if (b)
            return *b;
    return std::nullopt;
}

} // namespace

void FileResolver::ClearListings() {
    std::lock_guard lk(listings_.mu);
    listings_.map.clear();
}

std::shared_ptr<const FileResolver::Listing> FileResolver::ListDirectory(const fs::path& dir) const {
    const std::string key = PathToUtf8(dir);
    {
        std::lock_guard lk(listings_.mu);
        if (auto it = listings_.map.find(key); it != listings_.map.end())
            return it->second;
    }
    auto listing = std::make_shared<Listing>();
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        // Both follow symlinks, so a linked folder or file counts as one.
        std::error_code entryEc;
        const bool isDir = it->is_directory(entryEc);
        if (!isDir && !it->is_regular_file(entryEc))
            continue;
        std::string name = PathToUtf8(it->path().filename());
        listing->push_back({LowerAscii(name), std::move(name), isDir});
    }
    std::lock_guard lk(listings_.mu);
    // Another worker may have listed it meanwhile; either answer is the same.
    return listings_.map.emplace(key, std::move(listing)).first->second;
}

// Matches `rel` under `root` one component at a time, ignoring ASCII case.
// The last component may also match with any of `extensions` in place of its
// own. Empty when any component has no match.
fs::path FileResolver::ResolveCaseInsensitive(const fs::path& root, const fs::path& rel,
                                              std::span<const char* const> extensions) const {
    std::error_code ec;
    if (rel.empty() || !fs::is_directory(root, ec))
        return {};
    std::vector<fs::path> parts(rel.begin(), rel.end());
    fs::path cur = root;
    for (usize i = 0; i + 1 < parts.size(); ++i) {
        if (fs::is_directory(cur / parts[i], ec)) {
            cur /= parts[i];
            continue;
        }
        const std::string wanted[] = {LowerAscii(PathToUtf8(parts[i]))};
        const auto dir = MatchEntry(*ListDirectory(cur), wanted, /*wantDirectory=*/true);
        if (!dir)
            return {};
        cur /= FsPathFromUtf8(*dir);
    }
    std::vector<std::string> wanted{LowerAscii(PathToUtf8(parts.back()))};
    const std::string stem = LowerAscii(PathToUtf8(parts.back().stem()));
    for (const char* ext : extensions)
        wanted.push_back(stem + ext);
    const auto file = MatchEntry(*ListDirectory(cur), wanted, /*wantDirectory=*/false);
    if (!file)
        return {};
    // The listing may be stale; only a file that is still there is an answer.
    fs::path found = cur / FsPathFromUtf8(*file);
    return fs::is_regular_file(found, ec) ? found : fs::path{};
}

FileResolver::FileResolver(const fs::path& basePath) : basePath_(basePath) {}

std::string FileResolver::NormalizeSeparators(const std::string& path) {
    std::string out = path;
    for (auto& c : out)
        if (c == '\\')
            c = '/';
    return out;
}

fs::path FileResolver::Resolve(const std::string& relativePath,
                               std::span<const char* const> extensions) const {
    std::string norm = NormalizeSeparators(relativePath);

    fs::path relPath = FsPathFromUtf8(norm);
    fs::path filename = relPath.filename();

    struct Candidate {
        const fs::path* root;
        const fs::path* rel;
    };
    std::vector<Candidate> candidates;
    candidates.reserve(4 + assetRoots_.size());
    if (!basePath_.empty()) {
        candidates.push_back({&basePath_, &relPath});
        candidates.push_back({&basePath_, &filename});
    }
    for (const fs::path& root : assetRoots_)
        candidates.push_back({&root, &relPath});
    if (!systemBasePath_.empty()) {
        candidates.push_back({&systemBasePath_, &relPath});
        candidates.push_back({&systemBasePath_, &filename});
    }

    for (usize i = 0; i < candidates.size(); ++i) {
        const fs::path base = *candidates[i].root / *candidates[i].rel;

        if (fs::exists(base))
            return base;

        for (const char* ext : extensions) {
            fs::path alt = base;
            alt.replace_extension(ext);
            if (fs::exists(alt))
                return alt;
        }

        // Paths arrive lowercased (AssetManager::Normalize), which misses a
        // mixed-case Assets/Textures/Foo.dds on a case-sensitive filesystem.
        if (fs::path found = ResolveCaseInsensitive(*candidates[i].root, *candidates[i].rel, extensions);
            !found.empty())
            return found;
    }

    return {};
}

fs::path FileResolver::ResolveTexture(const std::string& relativePath) const {
    return Resolve(relativePath, kTextureExts);
}

fs::path FileResolver::ResolveModel(const std::string& relativePath) const {
    return Resolve(relativePath, kModelExts);
}

} // namespace whiteout::flakes::io
