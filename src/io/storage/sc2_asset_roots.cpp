#include "sc2_asset_roots.h"

#include "whiteout/flakes/types.h"
#include "whiteout/flakes/util/path_utf8.h"

#include <cctype>
#include <optional>
#include <string>
#include <string_view>

namespace whiteout::flakes::io {
namespace fs = std::filesystem;

namespace {

std::string LowerAscii(std::string s) {
    for (auto& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string LowerName(const fs::path& p) {
    return LowerAscii(PathToUtf8(p.filename()));
}

} // namespace

std::optional<fs::path> ChildFolderAnyCase(const fs::path& dir, const std::string& lowerName) {
    std::error_code ec;
    std::optional<std::string> best;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code entryEc;
        if (!it->is_directory(entryEc))
            continue;
        const std::string name = PathToUtf8(it->path().filename());
        if (LowerAscii(name) == lowerName && (!best || name < *best))
            best = name;
    }
    if (!best)
        return std::nullopt;
    return dir / FsPathFromUtf8(*best);
}

bool IsSc2DocumentName(const fs::path& p) {
    const std::string name = LowerName(p);
    for (const std::string_view suffix : {".sc2mod", ".sc2map", ".sc2campaign"})
        if (name.size() > suffix.size() && name.ends_with(suffix))
            return true;
    return false;
}

std::vector<fs::path> Sc2DocumentAssetRoots(const fs::path& docDir) {
    std::vector<fs::path> roots;
    if (const auto base = ChildFolderAnyCase(docDir, "base.sc2assets"); base && ChildFolderAnyCase(*base, "assets"))
        roots.push_back(*base);
    if (ChildFolderAnyCase(docDir, "assets"))
        roots.push_back(docDir);
    return roots;
}

std::vector<fs::path> FindSc2AssetRoots(const fs::path& modelPath) {
    const fs::path start = modelPath.parent_path();
    for (fs::path a = start; !a.empty(); a = a.parent_path()) {
        if (IsSc2DocumentName(a))
            return Sc2DocumentAssetRoots(a);
        if (a == a.parent_path())
            break;
    }
    // No enclosing mod: a bare extracted tree, `<root>/Assets/Models/x.m3`.
    for (fs::path a = start; !a.empty(); a = a.parent_path()) {
        if (LowerName(a) == "assets")
            return {a.parent_path()};
        if (a == a.parent_path())
            break;
    }
    return {};
}

} // namespace whiteout::flakes::io
