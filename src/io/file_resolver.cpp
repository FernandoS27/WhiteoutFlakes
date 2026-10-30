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

// The entry of `dir` whose lowercased name is one of `wanted`, earliest in
// `wanted` first; among same-name candidates, the smallest name by bytes so
// the pick does not depend on directory order.
std::optional<fs::path> MatchEntry(const fs::path& dir, std::span<const std::string> wanted, bool wantDirectory) {
    std::error_code ec;
    std::vector<std::optional<std::string>> best(wanted.size());
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        // Both follow symlinks, so a linked folder or file counts as one.
        std::error_code entryEc;
        const bool isDir = it->is_directory(entryEc);
        const bool isFile = !isDir && it->is_regular_file(entryEc);
        if (wantDirectory ? !isDir : !isFile)
            continue;
        const std::string name = PathToUtf8(it->path().filename());
        const std::string lower = LowerAscii(name);
        for (usize i = 0; i < wanted.size(); ++i)
            if (lower == wanted[i] && (!best[i] || name < *best[i]))
                best[i] = name;
    }
    for (const auto& b : best)
        if (b)
            return dir / FsPathFromUtf8(*b);
    return std::nullopt;
}

// Matches `rel` under `root` one component at a time, ignoring ASCII case.
// The last component may also match with any of `extensions` in place of its
// own. Empty when any component has no match.
fs::path ResolveCaseInsensitive(const fs::path& root, const fs::path& rel,
                                std::span<const char* const> extensions) {
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
        const auto dir = MatchEntry(cur, wanted, /*wantDirectory=*/true);
        if (!dir)
            return {};
        cur = *dir;
    }
    std::vector<std::string> wanted{LowerAscii(PathToUtf8(parts.back()))};
    const std::string stem = LowerAscii(PathToUtf8(parts.back().stem()));
    for (const char* ext : extensions)
        wanted.push_back(stem + ext);
    return MatchEntry(cur, wanted, /*wantDirectory=*/false).value_or(fs::path{});
}

} // namespace

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
    Candidate candidates[4];
    usize nCand = 0;
    if (!basePath_.empty()) {
        candidates[nCand++] = {&basePath_, &relPath};
        candidates[nCand++] = {&basePath_, &filename};
    }
    if (!systemBasePath_.empty()) {
        candidates[nCand++] = {&systemBasePath_, &relPath};
        candidates[nCand++] = {&systemBasePath_, &filename};
    }

    for (usize i = 0; i < nCand; ++i) {
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
