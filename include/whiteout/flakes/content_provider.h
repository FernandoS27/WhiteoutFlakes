#pragma once

/// @file content_provider.h
/// @brief Read-side abstraction the renderer uses to load MDX, BLP, and
///        referenced files. Reads are asynchronous: callers submit a
///        Request that produces a RequestId; the provider's worker thread
///        does the IO and a completion callback fires on the thread that
///        calls Pump(). Wait() and Cancel() act on a RequestId.

#include "types.h"

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace whiteout::flakes::io {

/// @brief Result delivered to a CompletionCallback. `ok` distinguishes a
///        successful read from a missing file, an empty file, or a
///        cancellation. `actualExt` is the lowercased extension of the
///        resolved file (e.g. `".dds"` when `.blp` was requested but
///        only `.dds` exists), so callers that decode by extension can
///        branch without re-parsing the path.
struct RequestResult {
    bool ok = false;
    std::vector<u8> data;
    std::string actualExt;
};

/// @brief Fires on the thread that invokes IContentProvider::Pump(),
///        never on the provider's worker thread — touching render-thread
///        state (gfx handles, texture caches, etc.) inside the callback
///        is safe.
using CompletionCallback = std::function<void(RequestResult&&)>;

/// @brief Opaque handle for cancel / wait. `kInvalidRequestId` is returned
///        when the provider rejects the request (e.g. empty path).
using RequestId = u64;
constexpr RequestId kInvalidRequestId = 0;

/// @brief Pluggable byte-stream source for asset reads.
///
/// Hosts implement this with a disk-backed, MPQ-backed (classic WC3),
/// CASC-backed (Reforged), or in-memory provider. The renderer never
/// touches the host filesystem directly — every asset lookup goes
/// through Request/Wait/Pump or the ReadFile compatibility wrapper.
class IContentProvider {
public:
    virtual ~IContentProvider() = default;

    /// @brief Submit a read. Safe to call from any thread. The callback
    ///        fires later on the thread that runs Pump().
    /// @param path Forward- or backslash-separated relative path (e.g.
    ///             `"Units/Human/Footman/Footman.mdx"`). Case-insensitive
    ///             matching is recommended since MDX files commonly mix
    ///             cases.
    /// @param cb   Completion callback. Receives the bytes (or a
    ///             not-ok result) and the resolved extension. Moved into
    ///             the worker queue.
    /// @return A RequestId usable with Wait()/Cancel(), or
    ///         kInvalidRequestId if the request was rejected outright.
    virtual RequestId Request(const std::string& path, CompletionCallback cb) = 0;

    /// @brief Block the caller until the given request has either fired
    ///        its callback or been cancelled. Must be called from the
    ///        same thread that calls Pump(); internally drains the
    ///        completion queue while waiting.
    virtual void Wait(RequestId id) = 0;

    /// @brief Best-effort cancel. If the worker has not started the
    ///        request, it is dropped silently. If it's already in flight
    ///        the IO completes but the callback does not fire. Safe to
    ///        call from any thread.
    virtual void Cancel(RequestId id) = 0;

    /// @brief Drain the completion queue on the calling thread, running
    ///        any pending callbacks. Hosts call this once per frame.
    virtual void Pump() = 0;

    /// @brief Synchronous convenience over Request+Wait. Existing
    ///        callers that need bytes-in-hand (BLS shader cache, MDX
    ///        parse, DNC, corn-effects) keep this surface; it must be
    ///        called from the Pump thread.
    std::optional<std::vector<u8>> ReadFile(const std::string& path,
                                            std::string* actualExt = nullptr);
};

} // namespace whiteout::flakes::io

namespace whiteout::flakes {
using ::whiteout::flakes::io::IContentProvider;
}
