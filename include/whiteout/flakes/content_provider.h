#pragma once

/// @file content_provider.h
/// @brief Read-side abstraction the renderer uses to load MDX, BLP, and
///        referenced files. Reads are asynchronous: callers submit a
///        Request that produces a RequestId; the provider's worker thread
///        does the IO and a completion callback fires on the thread that
///        calls Pump(). Wait() and Cancel() act on a RequestId.

#include "content_ref.h"
#include "enums.h" // Wc3ArtTier
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
    /// @param ref A path (forward- or backslash-separated, relative, e.g.
    ///            `"Units/Human/Footman/Footman.mdx"` — case-insensitive
    ///            matching is recommended since MDX files commonly mix
    ///            cases), or a CASC fileDataID. A provider that cannot
    ///            resolve fileDataIDs should return a not-ok result rather
    ///            than guessing; see FileContentProvider for the CASC route.
    /// @param cb  Completion callback. Receives the bytes (or a not-ok
    ///            result) and the resolved extension. Moved into the worker
    ///            queue.
    /// @return A RequestId usable with Wait()/Cancel(), or
    ///         kInvalidRequestId if the request was rejected outright.
    virtual RequestId Request(const ContentRef& ref, CompletionCallback cb) = 0;

    /// @brief Path-typed convenience. Every existing caller reaches the
    ///        provider this way and none of them changed.
    RequestId Request(const std::string& path, CompletionCallback cb) {
        return Request(ContentRef::FromPath(path), std::move(cb));
    }

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
    std::optional<std::vector<u8>> ReadFile(const ContentRef& ref,
                                            std::string* actualExt = nullptr);
    /// @overload Path-typed convenience, for the same reason Request has one.
    std::optional<std::vector<u8>> ReadFile(const std::string& path,
                                            std::string* actualExt = nullptr) {
        return ReadFile(ContentRef::FromPath(path), actualExt);
    }

    /// @brief List the files the provider knows about under @p directory.
    ///
    /// Used by bulk-preload paths that want "everything under
    /// `Textures/FX`" without the caller enumerating the archive itself.
    /// @param directory Provider-relative directory, `/` or `\` separated
    ///                  and case-insensitive (empty = every known file). An
    ///                  absolute path is answered from disk alone, in the
    ///                  same terms — see the return value.
    /// @param recursive Include files in nested subdirectories.
    /// @return Provider-relative paths, lowercased with `/` separators, each
    ///         readable via Request()/ReadFile() — or absolute ones when
    ///         @p directory was absolute. The default returns nothing — a
    ///         provider that cannot enumerate (the web fetch one, a host's own
    ///         callback shim) simply doesn't support directory preloads.
    virtual std::vector<std::string> ListFiles(const std::string& directory, bool recursive) {
        (void)directory;
        (void)recursive;
        return {};
    }

    /// @brief The fileDataID that names @p path, when the provider knows one.
    ///
    /// A World of Warcraft root is keyed by fileDataID and the client
    /// databases join on the same key, so anything that has to look a model up
    /// in them needs this — a path is not an identity the game recognises.
    /// Answering it needs a listfile, which is the same thing browsing that
    /// root needs, so a provider that can list one can usually answer this.
    /// @return 0 when nothing can say. Never a guess.
    virtual u32 FileIdForPath(const std::string& path) const {
        (void)path;
        return 0;
    }

    /// @brief The path that names @p fileId, when the provider knows one.
    ///
    /// The inverse of @ref FileIdForPath, and the only way a format whose
    /// asset graph is addressed by id can put a readable name on anything it
    /// references. A Diablo III AnimSet maps an animation tag to an Anim SNO
    /// and no shipped file names either one; the storage root does, because
    /// CoreTOC turns every id back into `Base\Anim\<name>.ani`.
    /// @return Empty when nothing can say — the same contract as
    ///         FileIdForPath, in the other direction.
    virtual std::string PathForFileId(u32 fileId) const {
        (void)fileId;
        return {};
    }

    /// @brief Which Warcraft III art tier subsequent reads resolve through.
    ///
    /// Warcraft III's storage is a chain of mod overlays, and the tier says
    /// which of them leads: `Classic` reads `war3.w3mod:`, `Reforged` puts
    /// `_hd.w3mod:` in front of it, `Definitive` puts `_de.w3mod:` in front of
    /// both. Each falls through to the older overlays, matching the game's own
    /// `W3Data::OpenMod` and its `-hd 0|1|2`.
    ///
    /// `Classic` by default. The host (viewer / plugin) flips this when the
    /// user picks a tier or opens a model from a known overlay, and owns any
    /// asset-cache invalidation that follows. A no-op for providers that do not
    /// layer archives, and meaningless for every other game — their storages
    /// have one namespace.
    virtual void SetArtTier(Wc3ArtTier tier) {
        (void)tier;
    }
    virtual Wc3ArtTier ArtTier() const {
        return Wc3ArtTier::Classic;
    }

    /// @brief The two-state view of @ref SetArtTier, for callers that only have
    ///        a render mode to go on.
    ///
    /// `true` selects `Reforged`, which is what "HD" meant before 3.0.0 added a
    /// third tier — so a caller that knows nothing about Definitive keeps
    /// reading exactly the art it used to. Ask for `Definitive` by name.
    void SetHdMode(bool enabled) {
        SetArtTier(enabled ? Wc3ArtTier::Reforged : Wc3ArtTier::Classic);
    }
    /// @brief Whether any HD-material tier is selected (Reforged or Definitive).
    bool HdMode() const {
        return ArtTier() != Wc3ArtTier::Classic;
    }
};

} // namespace whiteout::flakes::io

namespace whiteout::flakes {
using ::whiteout::flakes::io::IContentProvider;
}
