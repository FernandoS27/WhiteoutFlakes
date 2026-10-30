#include "sc2_document_source.h"

#include "io/file_resolver.h"
#include "sc2_asset_roots.h"
#include "storage_paths.h"
#include "whiteout/flakes/types.h"
#include "whiteout/flakes/util/path_utf8.h"

#include <cctype>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <set>

namespace whiteout::flakes::io {
namespace fs = std::filesystem;

namespace {

std::string LowerAscii(std::string s) {
    for (auto& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool ReadWholeFile(const fs::path& p, std::vector<u8>& out) {
    std::ifstream f(p, std::ios::binary);
    if (!f)
        return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return !f.bad();
}

} // namespace

Sc2DocumentSource::Sc2DocumentSource(fs::path doc, std::vector<fs::path> roots)
    : doc_(std::move(doc)), root_(PathToUtf8(doc_)), roots_(std::move(roots)) {}

std::unique_ptr<Sc2DocumentSource> Sc2DocumentSource::Open(const fs::path& doc, std::string& error) {
    if (!IsSc2DocumentName(doc)) {
        error = "not a .SC2Mod, .SC2Map or .SC2Campaign";
        return nullptr;
    }
    std::error_code ec;
    if (!fs::exists(doc, ec)) {
        error = "not found";
        return nullptr;
    }
    if (!fs::is_directory(doc, ec)) {
        error = "packed mods are not supported yet";
        return nullptr;
    }
    std::vector<fs::path> roots = Sc2DocumentAssetRoots(doc);
    if (roots.empty()) {
        error = "no Assets folder";
        return nullptr;
    }
    return std::unique_ptr<Sc2DocumentSource>(new Sc2DocumentSource(doc, std::move(roots)));
}

const Sc2DocumentSource::Index& Sc2DocumentSource::EnsureIndex() const {
    std::lock_guard lk(indexMu_);
    if (index_)
        return *index_;
    const auto start = std::chrono::steady_clock::now();
    Index index;
    for (const fs::path& root : roots_) {
        const auto assets = ChildFolderAnyCase(root, "assets");
        if (!assets)
            continue;
        // Symlinks are followed, so a linked-in folder counts. Walked by hand
        // rather than with recursive_directory_iterator, which gives up on the
        // whole walk at a symlink loop: here a folder already seen, or one
        // that will not open, only skips itself.
        std::set<fs::path> visited;
        std::vector<fs::path> pending{*assets};
        std::error_code ec;
        visited.insert(fs::weakly_canonical(*assets, ec));
        while (!pending.empty()) {
            const fs::path dir = std::move(pending.back());
            pending.pop_back();
            for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
                std::error_code entryEc;
                if (it->is_directory(entryEc)) {
                    if (visited.insert(fs::weakly_canonical(it->path(), entryEc)).second && !entryEc)
                        pending.push_back(it->path());
                    continue;
                }
                if (!it->is_regular_file(entryEc))
                    continue;
                // Lexical: fs::relative would resolve a linked folder away to
                // wherever it points, outside the mod.
                index.emplace(LowerAscii(it->path().lexically_relative(root).generic_string()), it->path());
            }
            ec.clear();
        }
    }
    const auto ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
    std::fprintf(stderr, "[io] fallback index: %s: %zu files (%lld ms)\n", root_.c_str(), index.size(),
                 static_cast<long long>(ms));
    index_ = std::move(index);
    return *index_;
}

bool Sc2DocumentSource::ReadKey(const std::string& key, SourceRead& out) const {
    const Index& index = EnsureIndex();
    auto hit = index.find(key);
    if (hit == index.end()) {
        const std::string ext = GetLowerExtension(key);
        auto [altExts, altCount] = AltExtensionsFor(ext);
        const std::string stem = StripExtension(key);
        for (usize i = 0; i < altCount && hit == index.end(); ++i)
            if (altExts[i] != ext)
                hit = index.find(stem + altExts[i]);
    }
    if (hit == index.end() || !ReadWholeFile(hit->second, out.data))
        return false;
    out.actualExt = GetLowerExtension(PathToUtf8(hit->second.filename()));
    return true;
}

bool Sc2DocumentSource::Read(const std::string& path, SourceRead& out) const {
    std::string key = LowerAscii(FileResolver::NormalizeSeparators(path));
    while (!key.empty() && key.front() == '/')
        key.erase(key.begin());
    // Base.SC2Assets' root was indexed first, so its copy already wins.
    return ReadKey(key, out);
}

void Sc2DocumentSource::List(const std::function<void(std::string)>& emit) const {
    for (const auto& [key, file] : EnsureIndex())
        emit(ToListingPath(key));
}

} // namespace whiteout::flakes::io
