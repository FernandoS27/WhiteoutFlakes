#include "io/fetch_content_provider.h"

#include <algorithm>
#include <cctype>
#include <utility>

namespace whiteout::flakes::io {

FetchContentProvider::FetchContentProvider()  = default;
FetchContentProvider::~FetchContentProvider() = default;

std::string FetchContentProvider::Normalize(std::string path) {
    for (auto& c : path) {
        if (c == '\\') c = '/';
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return path;
}

void FetchContentProvider::Put(const std::string& path, std::vector<u8> bytes) {
    std::lock_guard lk(mu_);
    cache_[Normalize(path)] = std::move(bytes);
}

std::size_t FetchContentProvider::CachedFileCount() const {
    std::lock_guard lk(mu_);
    return cache_.size();
}

RequestId FetchContentProvider::Request(const std::string& path, CompletionCallback cb) {
    if (path.empty() || !cb) return kInvalidRequestId;

    const RequestId id = nextId_.fetch_add(1, std::memory_order_relaxed);
    const std::string key = Normalize(path);

    // Walk the same alt-extension chains FileResolver uses for textures
    // and models, so a Request for `foo.tif` finds a cached `foo.dds`
    // automatically (the renderer asks via the path the MDX named, but
    // the loose file we got from the server may be a synonym format).
    static constexpr const char* kTextureExts[] = {".blp", ".dds", ".tga", ".png", ".tif"};
    static constexpr const char* kModelExts[]   = {".mdx", ".mdl"};
    auto extIn = [](const std::string& e, const char* const* arr, std::size_t n) {
        for (std::size_t i = 0; i < n; ++i)
            if (e == arr[i]) return true;
        return false;
    };

    RequestResult result;
    {
        std::lock_guard lk(mu_);
        auto it = cache_.find(key);
        if (it != cache_.end()) {
            result.ok = true;
            result.data = it->second; // copy — keeps the cache intact for repeats
            auto dot = key.rfind('.');
            if (dot != std::string::npos) result.actualExt = key.substr(dot);
        } else {
            // Miss on the exact key — try alt-ext synonyms before giving up.
            // Match FileResolver::Resolve, which tries `basePath / relPath`
            // AND `basePath / filename` before walking extensions. Here
            // that's: walk alt-exts on the full path stem, then on the
            // filename-only stem (everything after the last '/').
            const auto dot = key.rfind('.');
            bool foundAlt = false;
            if (dot != std::string::npos) {
                const std::string stemFull = key.substr(0, dot);
                const std::string reqExt   = key.substr(dot);
                const auto slash = stemFull.rfind('/');
                const std::string stemBase =
                    (slash != std::string::npos) ? stemFull.substr(slash + 1) : stemFull;

                const char* const* alts = nullptr;
                std::size_t altsN = 0;
                if (extIn(reqExt, kTextureExts, std::size(kTextureExts))) {
                    alts = kTextureExts; altsN = std::size(kTextureExts);
                } else if (extIn(reqExt, kModelExts, std::size(kModelExts))) {
                    alts = kModelExts; altsN = std::size(kModelExts);
                }
                const std::string stems[2] = { stemFull, stemBase };
                const std::size_t nStems = (stemFull == stemBase) ? 1 : 2;
                for (std::size_t s = 0; alts && s < nStems && !foundAlt; ++s) {
                    for (std::size_t i = 0; i < altsN; ++i) {
                        auto it2 = cache_.find(stems[s] + alts[i]);
                        if (it2 != cache_.end()) {
                            result.ok = true;
                            result.data = it2->second;
                            result.actualExt = alts[i];
                            foundAlt = true;
                            break;
                        }
                    }
                }
            }
            if (!foundAlt) {
                // Miss — the caller will see ok=false and handle the
                // fallback itself. AssetManager paths are pushed via
                // ApplyPrepared, not through ReadFile, so these misses
                // are init-time / SLK / sound paths that the host has
                // already pre-staged via _putBytes if they exist.
                if (dot != std::string::npos) result.actualExt = key.substr(dot);
            }
        }

        completed_.push_back(Pending{id, std::move(cb), std::move(result)});
    }
    return id;
}

void FetchContentProvider::Wait(RequestId id) {
    // Every Request enqueues a completion synchronously, so Wait just
    // needs to drain — the requested id is guaranteed to be in
    // `completed_` already (or will never arrive, in which case the
    // caller violated the lifetime contract).
    (void)id;
    Pump();
}

void FetchContentProvider::Cancel(RequestId /*id*/) {
    // No-op: requests resolve synchronously inside Request(), so by the
    // time anyone could call Cancel the callback either has fired (via
    // Pump) or is sitting in the queue. We let Pump retire it normally.
}

void FetchContentProvider::Pump() {
    std::deque<Pending> batch;
    {
        std::lock_guard lk(mu_);
        batch.swap(completed_);
    }
    for (auto& p : batch) {
        if (p.cb) p.cb(std::move(p.result));
    }
}

} // namespace whiteout::flakes::io
