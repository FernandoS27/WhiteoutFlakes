#include "casc_source.h"

#if WHITEOUT_HAS_CASC

#include "storage_paths.h"

#include <whiteout/utils/simple_thread_pool.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <optional>

namespace whiteout::flakes::io {

namespace {

namespace casc = whiteout::storages::casc;

constexpr const char* kPrefixSd = "war3.w3mod:";
constexpr const char* kPrefixHd = "war3.w3mod:_hd.w3mod:";
constexpr const char* kPrefixDeprecated = "war3.w3mod:_deprecated.w3mod:";
constexpr const char* kPrefixNone = "";

// Warcraft III's chain, ordered by the HD toggle. Every other product gets the
// single empty prefix — prefixing a bare-path root would turn each read into
// three misses before the one that was always going to work.
std::array<const char*, 4> Prefixes(const std::atomic<bool>* hdMode) {
    if (!hdMode)
        return {kPrefixNone, nullptr, nullptr, nullptr};
    if (hdMode->load(std::memory_order_relaxed))
        return {kPrefixHd, kPrefixSd, kPrefixDeprecated, kPrefixNone};
    return {kPrefixSd, kPrefixHd, kPrefixDeprecated, kPrefixNone};
}

} // namespace

CascSource::CascSource(std::string root, casc::Storage storage, const CascSourceOptions& opts)
    : root_(std::move(root)), storage_(std::move(storage)), hdMode_(opts.hdMode),
      fileIds_(opts.fileIds), frameSuffixFallback_(opts.frameSuffixFallback) {}

std::unique_ptr<CascSource> CascSource::Open(std::string root, const CascSourceOptions& opts,
                                             std::string& error) {
    if (root.empty()) {
        error = "no install path";
        return nullptr;
    }
    casc::OpenOptions co;
    co.path = root;
    co.pool = opts.pool;
    co.errorOut = &error;
    if (!opts.listfile.empty())
        co.listfile = opts.listfile;

    auto storage = casc::Storage::open(co);
    if (!storage)
        return nullptr;
    // Before the first read, and not conditional on the file being there: a
    // storage with no keys still reads everything unencrypted, which is most
    // of an install.
    bool keys = false;
    if (!opts.tactKeyFile.empty()) {
        keys = storage->importKeysFromFile(opts.tactKeyFile);
        if (!keys)
            std::printf("[FileContentProvider] TACT keys not readable: %s\n",
                        opts.tactKeyFile.c_str());
    }
    storage->setZeroFillEncrypted(opts.zeroFillEncrypted);
    std::printf("[FileContentProvider] CASC storage opened: %s%s%s\n", root.c_str(),
                opts.listfile.empty() ? "" : " (with listfile)", keys ? " (with TACT keys)" : "");
    return std::unique_ptr<CascSource>(new CascSource(std::move(root), std::move(*storage), opts));
}

bool CascSource::ReadStem(const std::string& stem, const std::string& ext, SourceRead& out) const {
    auto [altExts, altCount] = AltExtensionsFor(ext);
    for (const char* prefix : Prefixes(hdMode_)) {
        if (!prefix)
            break; // a bare-path root declares one prefix, not four
        if (!ext.empty()) {
            auto data = storage_.readFile(std::string(prefix) + stem + ext);
            if (data && !data->empty()) {
                out.actualExt = ext;
                out.data = std::move(*data);
                return true;
            }
        }
        for (usize i = 0; i < altCount; ++i) {
            if (altExts[i] == ext)
                continue;
            auto data = storage_.readFile(std::string(prefix) + stem + altExts[i]);
            if (data && !data->empty()) {
                out.actualExt = altExts[i];
                out.data = std::move(*data);
                return true;
            }
        }
    }
    return false;
}

bool CascSource::Read(const std::string& path, SourceRead& out) const {
    const std::string norm = NormalizeCascPath(path);
    const std::string stem = StripExtension(norm);
    const std::string ext = GetLowerExtension(norm);

    if (ReadStem(stem, ext, out))
        return true;

    if (frameSuffixFallback_) {
        const auto dash = stem.rfind('-');
        if (dash != std::string::npos && dash + 1 < stem.size() &&
            std::all_of(stem.begin() + dash + 1, stem.end(),
                        [](unsigned char ch) { return std::isdigit(ch) != 0; })) {
            return ReadStem(stem.substr(0, dash), ext, out);
        }
    }
    return false;
}

// The locale walk is not belt-and-braces. A WoW fileDataID resolves to one
// root entry PER LOCALE — 13 of them for a stock localised file — and only the
// installed locale's data is actually on disk. `readFile(id)` with no locale
// mask takes whichever entry the root manifest happens to list first, which is
// the wrong one on any install that is not the manifest's first locale, and
// returns a miss because that archive was never downloaded. Worse, the order
// is not stable: opening with a worker pool (which this always does) parses
// the manifest in parallel and lists them differently, so the same id reads
// fine single-threaded and misses through the provider.
//
// Asking for a specific locale sidesteps all of it — the storage then selects
// by mask rather than by position. The unmasked attempt comes first because it
// is right and free whenever an id has a single entry, which is the case for
// models and textures; the walk only runs after a miss, and only one locale
// can have data.
bool CascSource::ReadById(u32 fileId, SourceRead& out) const {
    if (!fileIds_)
        return false;
    namespace L = casc::LocaleMasks;
    // Fixed order, so one machine always answers the same way. Which entry
    // wins still varies BETWEEN machines, and correctly so — that is what a
    // locale is.
    static constexpr u32 kLocales[] = {
        L::enUS, L::enGB, L::deDE, L::frFR, L::esES, L::esMX, L::ruRU, L::ptBR, L::ptPT, L::itIT,
        L::plPL, L::koKR, L::zhCN, L::zhTW, L::enCN, L::enTW, L::jaJP, L::thTH, L::trTR,
    };
    const i32 id = static_cast<i32>(fileId);
    std::optional<std::vector<u8>> data = storage_.readFile(id);
    for (u32 locale : kLocales) {
        if (data && !data->empty())
            break;
        data = storage_.readFile(id, locale);
    }
    if (!data || data->empty())
        return false;
    out.data = std::move(*data);
    // Deliberately empty: the root manifest is id-keyed, so there is no name
    // to take an extension from. Consumers that decode by extension must know
    // the kind from the reference that produced the id (an M2's `.skin` slot,
    // a texture slot), which is exactly how the formats that use ids work.
    out.actualExt.clear();
    return true;
}

// Longest suffix wins, and that is the whole subtlety. A listfile names files
// relative to the storage root, but a model can perfectly well be opened from a
// loose extraction of that same install — an absolute Windows path with the
// storage-relative part on the end. Trying successive suffixes finds it; trying
// the *longest* one first is what stops a bare `cow.m2` from matching a
// different creature's file of the same name.
u32 CascSource::FileIdForPath(const std::string& path) const {
    if (!fileIds_)
        return 0;
    const std::string norm = NormalizeCascPath(path);
    for (usize at = 0; at < norm.size();) {
        auto info = storage_.fileInfo(norm.substr(at));
        if (info && info->fileDataId > 0)
            return static_cast<u32>(info->fileDataId);
        const usize sep = norm.find_first_of("/\\", at);
        if (sep == std::string::npos)
            break;
        at = sep + 1;
    }
    return 0;
}

void CascSource::List(const std::function<void(std::string)>& emit) const {
    storage_.enumerate([&](const casc::EnumerateEntry& e) {
        emit(ToListingPath(e.path));
        return true;
    });
}

} // namespace whiteout::flakes::io

#endif // WHITEOUT_HAS_CASC
