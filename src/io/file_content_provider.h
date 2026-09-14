#pragma once

#include "whiteout/flakes/content_provider.h"
#include "whiteout/flakes/enums.h" // ProductId
#include "whiteout/flakes/types.h"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace whiteout::flakes::io {

class ProgressMonitor;

/// Where the active game's storages are in their life.
///
/// Only Open and Failed are answers a read acts on. The other three exist for
/// the host: Dirty and Opening are the difference between "nothing has needed
/// one yet" and "one is being opened right now", which a status display must
/// tell apart or it reports "pending" for the whole of a forty-second open;
/// and Cancelled is what makes a cancelled open retryable when a failed one
/// is not.
enum class StorageState : u8 {
    Dirty,     ///< Configured but not built. The next demand opens it.
    Opening,   ///< A build is in flight, on some thread.
    Open,      ///< Built, with at least one source.
    Failed,    ///< Built and empty, or the open errored. Not retried on reads.
    Cancelled, ///< A caller stopped the open. Retried only when asked explicitly.
};

class FileContentProvider : public IContentProvider {
public:
    FileContentProvider();
    ~FileContentProvider();

    FileContentProvider(FileContentProvider&&) noexcept;
    FileContentProvider& operator=(FileContentProvider&&) noexcept;

    FileContentProvider(const FileContentProvider&) = delete;
    FileContentProvider& operator=(const FileContentProvider&) = delete;

    void SetBasePath(const std::filesystem::path& basePath);

    // Root for engine-shipped assets (the BLS `shaders/` pack, pso_trace.bin).
    // The ctor defaults it to the executable's directory, which is only right
    // when the host IS an executable shipped beside those files. Hosts loaded
    // as a shared library — language bindings, plugins — have to point this
    // somewhere real themselves.
    void SetSystemBasePath(const std::filesystem::path& root);

    // ---- IContentProvider async surface ----
    // The only provider that can resolve a fileDataID: it owns the
    // casc::Storage handle, and readFile(i32, FileIdHint) is already there.
    RequestId Request(const ContentRef& ref, CompletionCallback cb) override;
    using IContentProvider::Request;
    void Wait(RequestId id) override;
    void Cancel(RequestId id) override;
    void Pump() override;
    std::vector<std::string> ListFiles(const std::string& directory, bool recursive) override;
    // Needs the same listfile ListFiles does — see IContentProvider.
    u32 FileIdForPath(const std::string& path) const override;
    std::string PathForFileId(u32 fileId) const override;

    // Both open the current game's storages if a reconfiguration left them
    // deferred — the question they answer cannot be answered otherwise.
    bool HasCasc() const;

    bool HasMpq() const;

    // True when configuration changed and nothing has needed a storage since,
    // so none is open *yet*. Distinguishes that from "opening failed", which
    // HasCasc() alone reports identically. A status display should ask this
    // first; asking HasCasc() is what forces the open it is reporting on.
    bool StoragesPending() const;

    // True while a build is in flight. Answered from an atomic, WITHOUT the
    // storage lock — which is the point: an open holds that lock for its whole
    // duration, so a host that asked HasCasc() to draw its status line would
    // block on the very operation it is trying to report.
    //
    // Any host drawing storage status must check this (and StoragesPending)
    // before it asks anything else here.
    bool StoragesOpening() const;

    // The full state, for a host that wants to say *why* nothing is open.
    StorageState StoragesState() const;

    // Open the active game's storages now, reporting through @p progress.
    //
    // This is the operation StoragesPending() has always been describing, made
    // callable: hosts run it as a background task so the seconds of index and
    // manifest parsing happen off the thread that draws. Safe from any thread.
    // Returns true when the storages ended up usable.
    //
    // Unlike a read, this retries a Cancelled open — asking explicitly is what
    // distinguishes "the user wants to try again" from "the load that was
    // waiting behind the cancel would restart it immediately", which would
    // make Cancel mean nothing.
    bool OpenStorages(ProgressMonitor* progress = nullptr);

    // ---- Which game's content layout this provider serves ----
    //
    // Three products with genuinely different storage rules, so this selects
    // more than a search path:
    //
    //   Wc3  CASC (war3.w3mod mod-prefix chain) + the three War3*.mpq archives.
    //   Wow  CASC, optionally enriched by a listfile because a WoW root is
    //        id-keyed and carries no readable paths; fileDataID reads; and for
    //        pre-Warlords installs a Data/ MPQ set whose names vary by version.
    //   Sc2  CASC only — and up to *two* of them, because StarCraft II and
    //        Heroes of the Storm are separate installs sharing one render
    //        profile (see ProductIdFromBuildProduct, where `hero` maps to Sc2).
    //
    // Defaults to Wc3, which is what keeps every existing host unchanged.
    //
    // Every product has its own install root, archive list, listfile, ignore
    // switches and open storages, so setting this closes nothing and opens
    // nothing: it selects which set the reads and the settings below apply to.
    // A game that has been visited keeps what it opened until its own
    // configuration changes or the provider is destroyed, which is what makes
    // clicking through a settings panel — or a scene following the game a
    // loaded model turned out to belong to — free after the first visit.
    ProductId Game() const;
    void SetGame(ProductId game);

    // Auto-detected install root for @p game, from the ctor's game-finder scan.
    // Empty when that product was not found. Read-only; never reflects user
    // overrides. `Heroes` is reached through @ref HotsPath, not here — it
    // shares ProductId::Sc2 with StarCraft II and both can be installed.
    std::string GamePath(ProductId game) const;
    const std::string& HotsPath() const;

    // Heroes of the Storm's *active* root — what HotsPath is to Wc3Path,
    // this is to InstallPath. It gets its own override because ProductId::Sc2
    // covers two separate installs, so one install path cannot name both.
    // Empty reverts to the discovered path. Read by the Sc2 product only.
    std::string HotsInstallPath() const;
    void SetHotsInstallPath(const std::string& path);

    // Roots of the CASC storages that actually opened, in search order.
    // HasCasc() answers "any", which is enough when there is only ever one;
    // StarCraft II offers two roots of which either may fail, and a bool
    // cannot say which one did.
    std::vector<std::string> OpenCascRoots() const;

    // Auto-detected Warcraft III install root from the ctor's blizzard_game_finder
    // scan. Read-only; never reflects user overrides.
    const std::string& Wc3Path() const;

    // ---- Listfile (WoW) ----
    //
    // A WoW CASC root names files by fileDataID and a name hash, not by path,
    // so nothing can browse or path-read it without an external mapping.
    // Community listfiles are `id;path` CSV. Loading one is what turns a WoW
    // storage from "id reads only" into something ListFiles() can walk.
    //
    // Belongs to the current game and is applied on its next storage open, so
    // set it after SetGame. Empty path clears it.
    void SetListfilePath(const std::filesystem::path& csv);
    std::string ListfilePath() const;
    bool HasListfile() const;

    // ---- TACT keys (WoW) ----
    //
    // The listfile's twin, one layer down. Blizzard encrypts individual frames
    // of shipped files with per-content keys, and there is no error a caller
    // can see: the encoding lives inside the container, so a file with one
    // encrypted frame reads back as *missing*. Community key lists are
    // `keyName keyHex` per line.
    //
    // Supplying one also turns on zero-fill for the frames whose keys are still
    // unpublished (unreleased content), because a client database that is 99%
    // readable beats none of it. Same slot rules as the listfile: belongs to
    // the current game, applied on its next storage open.
    void SetTactKeyPath(const std::filesystem::path& keyList);
    std::string TactKeyPath() const;

    // Currently-active install root. Both CASC and the MPQ list search from
    // this directory. Defaults to Wc3Path(); pass an empty string to revert.
    // Returned by value because reconfiguration from another thread could
    // invalidate the underlying string between getter call and use.
    std::string InstallPath() const;
    void SetInstallPath(const std::string& path);

    // Per-storage enable switches. When set true, the corresponding storage
    // is closed and ReadFile() skips that fallback branch entirely.
    bool IgnoreCasc() const;
    bool IgnoreMpq() const;
    void SetIgnoreCasc(bool ignore);
    void SetIgnoreMpq(bool ignore);

    // MPQ load order. Each entry is a filename looked up under InstallPath().
    // Earlier entries are searched first when ReadFile() walks the chain.
    // Empty list = no MPQs loaded. Returned by value for the same reason as
    // InstallPath().
    std::vector<std::string> MpqList() const;
    void SetMpqList(std::vector<std::string> list);

    // Built-in MPQ load order used when no user override is set. Exposed so
    // the host UI can wire a "reset to defaults" button. The no-argument form
    // is Warcraft III's, kept because every existing caller means that.
    static std::vector<std::string> DefaultMpqList();
    static std::vector<std::string> DefaultMpqList(ProductId game);

    // What is actually on disk under InstallPath() for the current game, in
    // load order. WoW is why this exists: its archive names change with every
    // expansion (common/patch-N → art/expansionN/wow-update-base-N) and are
    // split across `Data/` and a per-locale subdirectory, so a hardcoded list
    // is wrong for most installs. Scanning reports what is there instead of
    // asserting what should be. Returns the static default for products whose
    // names genuinely are fixed.
    std::vector<std::string> ScanMpqList() const;

    // ---- HD mod overlay ----
    // Picks which of Warcraft III's mod overlays leads the CASC prefix
    // chain. Classic by default; the host calls this when the user picks a
    // tier, or when it opens a model out of a known overlay.
    void SetArtTier(Wc3ArtTier tier) override;
    Wc3ArtTier ArtTier() const override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace whiteout::flakes::io
