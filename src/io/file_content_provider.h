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

    // Both open the storages if a reconfiguration left them deferred — the
    // question they answer cannot be answered otherwise.
    bool HasCasc() const;

    bool HasMpq() const;

    // True when configuration changed and nothing has needed a storage since,
    // so none is open *yet*. Distinguishes that from "opening failed", which
    // HasCasc() alone reports identically. A status display should ask this
    // first; asking HasCasc() is what forces the open it is reporting on.
    bool StoragesPending() const;

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
    // Setting it reopens the storages.
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
    // Applied on the next storage open, so it is set before SetGame/SetInstallPath
    // or those are re-run. Empty path clears it.
    void SetListfilePath(const std::filesystem::path& csv);
    std::string ListfilePath() const;
    bool HasListfile() const;

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
    // Reorders CASC mod-prefix iteration so `_hd.w3mod` is tried
    // before the SD base when enabled. Off by default; the host
    // calls this whenever the user picks HD vs SD render mode.
    void SetHdMode(bool enabled) override;
    bool HdMode() const override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace whiteout::flakes::io
