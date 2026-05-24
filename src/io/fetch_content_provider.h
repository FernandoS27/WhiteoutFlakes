#pragma once

/// @file fetch_content_provider.h
/// @brief Web-targeted IContentProvider backed by an in-memory map that
///        JS populates via `wf_provider_put`. Reads against the map
///        complete synchronously (no thread, no condvar). For a cold
///        miss the Request returns a "not found" completion immediately
///        so Wait() / ReadFile() never block — JS is expected to have
///        pre-fetched every asset the renderer needs before calling
///        into wf_init / wf_spawn_unit.

#include "whiteout/flakes/content_provider.h"

#include <atomic>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace whiteout::flakes::io {

class FetchContentProvider final : public IContentProvider {
public:
    FetchContentProvider();
    ~FetchContentProvider() override;

    FetchContentProvider(const FetchContentProvider&) = delete;
    FetchContentProvider& operator=(const FetchContentProvider&) = delete;

    /// @brief Ingest a file's bytes. Called by the JS facade for every
    ///        asset prefetched before `wf_init` / `wf_spawn_unit`. Path
    ///        is normalised (lowercase, forward slashes) before storing.
    void Put(const std::string& path, std::vector<u8> bytes);

    /// @brief Number of files in the cache (for diagnostics from JS).
    std::size_t CachedFileCount() const;

    // IContentProvider ------------------------------------------------------
    RequestId Request(const std::string& path, CompletionCallback cb) override;
    void Wait(RequestId id) override;
    void Cancel(RequestId id) override;
    void Pump() override;

private:
    struct Pending {
        RequestId id;
        CompletionCallback cb;
        RequestResult result;
    };

    static std::string Normalize(std::string path);

    mutable std::mutex mu_;
    std::unordered_map<std::string, std::vector<u8>> cache_;
    std::deque<Pending> completed_;
    std::atomic<u64> nextId_{1};
};

} // namespace whiteout::flakes::io
