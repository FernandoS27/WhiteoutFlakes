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

    RequestResult result;
    {
        std::lock_guard lk(mu_);
        auto it = cache_.find(key);
        if (it != cache_.end()) {
            result.ok = true;
            result.data = it->second; // copy — keeps the cache intact for repeats
        }
        // Extension is the suffix after the last '.' in the normalised key.
        auto dot = key.rfind('.');
        if (dot != std::string::npos) result.actualExt = key.substr(dot);

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
