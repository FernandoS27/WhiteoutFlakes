#pragma once

// StorageExplorer — the embeddable model/storage explorer panel. Browses a CASC
// archive and renders live model/effect thumbnails, each in its own
// RenderService scene + offscreen viewport. Owns NO window / gfx device / ImGui
// context: a host supplies all three plus a shared RenderService, then drives
// the per-frame contract each frame, in order:
//
//   NewFrame(dt)         — pump the explorer's provider + apply staged nav
//   BuildWindow(&open)   — build the panel window (emits ImGui::Image cmds)
//   RenderThumbnails(dt) — render visible cells into their targets (settings
//                          guarded so the host's main view is untouched)
//
// See STORAGE_EXPLORER_DESIGN.md.

#include "io/storage_browser.h"

namespace whiteout::flakes::io {
class LoadTaskRunner;
}
#include "thumbnail_pool.h"

#include "renderer/render_service.h"
#include "renderer/types.h"
#include "whiteout/flakes/gfx_types.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace whiteout::flakes::io {
class FileContentProvider;
class IContentProvider;
} // namespace whiteout::flakes::io

namespace whiteout::flakes::tools {

// Concrete type of an activated file (a model dialect or an effect dialect).
enum class StorageFileKind { Mdx, Mdl, Pkb, Pkfx, M2, M3 };

// Handed to the activate callback when a file is double-clicked. `path` (the
// CASC-native archive path) and `provider` (the storage it lives in) are always
// set; `bytes` is filled only when DeliverBytes() is on — the panel reads the
// file synchronously and hands the consumer the content directly.
struct ActivatedFile {
    std::string path;
    StorageFileKind kind = StorageFileKind::Mdx;
    bool isEffect = false; // kind is Pkb/Pkfx
    std::shared_ptr<io::IContentProvider> provider;
    bool hasBytes = false;
    std::vector<std::uint8_t> bytes;
};

// Everything a host knows about where one product's storage lives: its install
// override, plus the listfile and TACT key list. An empty `installPath` means
// "whatever this machine has detected".
struct GameStorageKeys {
    std::string installPath;
    std::string listfilePath;
    std::string tactKeyPath;
};

class StorageExplorer {
public:
    // Borrows the host's RenderService. The gfx device must already be up — the
    // ctor creates the thumbnail pool, which allocates offscreen targets.
    explicit StorageExplorer(renderer::RenderService& svc);
    ~StorageExplorer();

    StorageExplorer(const StorageExplorer&) = delete;
    StorageExplorer& operator=(const StorageExplorer&) = delete;

    // Open a CASC archive root; points the explorer's OWN provider at it and
    // clears the thumbnail pool. Returns false + fills LastError() on failure.
    bool OpenCasc(const std::string& root);
    // The panel/provider state an open implies, applied once the browser's
    // tree is complete. Host thread only — it touches the thumbnail pool's
    // GPU resources and the provider's configuration.
    void FinishOpenCasc(const std::string& root);
    bool IsOpen() const {
        return browser_.IsOpen();
    }
    // The open succeeded but enumerated nothing browsable — a different state
    // from !IsOpen(), and the one an id-keyed World of Warcraft root opened
    // without a listfile lands in.
    bool IsEmpty() const {
        return openedEmpty_;
    }
    // Jump the browser to a folder (display form, '\\'-separated, "" = root) and
    // clear the thumbnail pool. Lets a host deep-link into a folder on open.
    void NavigateTo(const std::string& displayPath);
    const std::string& Root() const {
        return browser_.Root();
    }
    const std::string& LastError() const {
        return lastError_;
    }

    // Where a storage open should run. Injected rather than owned, for the
    // same reason the game keys are asked for rather than pushed: a host
    // with its own runner draws ONE progress modal, and a panel that made
    // its own would put a second one behind it.
    //
    // Unset (the default) keeps the old synchronous open, which is what the
    // headless self-tests want — there is no frame loop there to poll a bar.
    void SetTaskRunner(io::LoadTaskRunner* runner) {
        tasks_ = runner;
    }

    // An open is in flight on the task thread. The panel draws a placeholder
    // instead of the listing while this is true: the browser's tree is being
    // built from another thread and is not safe to walk.
    bool Opening() const {
        return opening_;
    }

    // ---- Per-frame host contract (call in this order) ----
    void NewFrame(float dt);
    void BuildWindow(bool* pOpen);
    void RenderThumbnails(float dt);

    // Archive path (CASC-native separators) of the last single-clicked file.
    const std::string& Selected() const {
        return selectedPath_;
    }

    // ---- Which game, and which of its file types ----
    //
    // The two are one control: what a filter even *means* depends on the game
    // open, so the panel offers a game combo and a checkbox per type that game
    // ships (io::BrowseTypesFor). Picking a game opens its detected install.
    // Returns false and fills LastError() when that install is not there.
    bool OpenGame(ProductId game);
    ProductId Game() const {
        return browser_.Product();
    }

    void SetBrowseTypes(io::BrowseType types) {
        browser_.SetEnabledTypes(types);
    }
    io::BrowseType BrowseTypes() const {
        return browser_.EnabledTypes();
    }
    // Show the game combo + type checkboxes in the panel so the end-user can
    // switch too (default true). Hide it to lock what SetBrowseTypes chose. The
    // search box and zoom below are part of the grid itself and stay either way.
    void SetFilterUIVisible(bool on) {
        filterUiVisible_ = on;
    }

    // Free-text filter over the current folder (io::MatchesFilter syntax:
    // substrings, `*`/`?` globs, comma-separated alternatives, `-` to exclude).
    // Same box the panel's own search field drives; applied at once (typing in
    // the box is what waits — see kSearchDebounce).
    void SetSearchText(const std::string& text);
    const char* SearchText() const {
        return searchText_;
    }

    // Grid icon edge length in logical pixels, clamped to [48, 320]. Also the
    // size each thumbnail RENDERS at, so shrinking the icons makes a screenful
    // cheaper rather than merely denser. The user drives this with the panel's
    // zoom slider or Ctrl+wheel over the grid.
    void SetIconSize(float px);
    float IconSize() const {
        return iconSize_;
    }

    // Listfile and TACT key list to open a World of Warcraft install with. Its
    // root is id-keyed: without a listfile a browse of it is *empty*, not
    // merely unnamed. Passing what the host's own provider uses also means the
    // panel shares that storage rather than opening a second one. Applied on
    // the next open.
    void SetCascKeys(std::string listfilePath, std::string tactKeyPath) {
        listfilePath_ = std::move(listfilePath);
        tactKeyPath_ = std::move(tactKeyPath);
        browser_.SetCascKeys(listfilePath_, tactKeyPath_);
    }

    // Where those settings come from, ASKED at the moment a product is opened
    // rather than pushed once. A host acquires them at times it has no reason
    // to tell the panel about — a listfile adopted beside a loose model, an
    // install path typed into its own settings — and they are per product,
    // which the panel's game combo can switch independently of the host. Both
    // are why a one-shot SetCascKeys goes stale: it captures one product's
    // state at one instant. Optional; a host that knows its keys up front can
    // keep calling SetCascKeys.
    using GameKeysCb = std::function<GameStorageKeys(ProductId)>;
    void SetGameKeys(GameKeysCb cb) {
        gameKeys_ = std::move(cb);
    }

    // Reconcile the panel with the host's storage settings. Cheap, idempotent,
    // and meant to be called every time the panel is shown:
    //  - nothing open yet -> open `fallback` (the host's current product);
    //  - already open     -> re-resolve THAT product's keys and reopen only if
    //                        they moved.
    // Where an open panel points is the user's own business (its game combo and
    // folder picker both write it), so Sync never moves it — a close/reopen
    // keeps the folder they were in, while a listfile acquired in between still
    // takes effect.
    void Sync(ProductId fallback);

    // Whether the activate callback receives the file's bytes (DeliverBytes
    // true — the panel reads the file synchronously) or just its path + provider
    // (false, the default — the consumer reads it however it wants, or not).
    void SetDeliverBytes(bool on) {
        deliverBytes_ = on;
    }
    bool DeliverBytes() const {
        return deliverBytes_;
    }

    // Fired on a model/effect double-click ("open this"). See ActivatedFile.
    using ActivateCb = std::function<void(const ActivatedFile&)>;
    void SetOnActivate(ActivateCb cb) {
        onActivate_ = std::move(cb);
    }

    // The CASC-backed provider the panel reads through.
    std::shared_ptr<io::IContentProvider> Provider() const;

private:
    void BuildGrid();      // breadcrumb + folder/file grid (inside the open window)
    void BuildFilterBar(); // game combo + one checkbox per browsable type
    void BuildSearchBar(); // search box + match count + zoom
    void BuildEmptyHint(); // why an opened storage enumerated nothing
    void OpenCascDialog(); // native folder picker → OpenCasc
    // What the host says @p game's storage is, with the detected install filled
    // in where it named none. Resolves only — opens nothing.
    GameStorageKeys ResolveGame(ProductId game) const;

    renderer::RenderService& svc_;
    io::StorageBrowser browser_;
    io::LoadTaskRunner* tasks_ = nullptr;
    // Written on the host thread only (set before submitting, cleared in the
    // completion), so the panel can test it without synchronisation.
    bool opening_ = false;
    // The task body's error slot. Not shared with lastError_: that one is
    // read by the panel every frame, including while the task is running.
    std::string openError_;
    std::shared_ptr<io::FileContentProvider> provider_;
    std::unique_ptr<ThumbnailPool> pool_;

    std::uint64_t frameCounter_ = 0;
    std::string selectedPath_;
    std::string lastError_;
    ActivateCb onActivate_;
    // Kept so a game switch can re-apply them: they belong to the provider's
    // per-product slot, not to one open.
    std::string listfilePath_;
    std::string tactKeyPath_;
    GameKeysCb gameKeys_;
    // The root the last successful open was asked for — not browser_.Root(),
    // which is what the storage resolved it to (a `Data/` suffix, a normalised
    // form). Sync reopens with it, so it has to be the request, not the answer.
    std::string openedRoot_;
    // The open succeeded and enumerated nothing browsable. A distinct state
    // from "no storage open": it is what an id-keyed World of Warcraft root
    // opened without a listfile looks like, and left unsaid it reads as an
    // empty folder rather than as a missing setting.
    bool openedEmpty_ = false;
    bool filterUiVisible_ = true;
    bool deliverBytes_ = false;

    // Search box contents (a plain buffer: ImGui's InputText owns the editing,
    // and the browser is told about it once per frame). `focusSearch_` is set by
    // Ctrl+F and consumed by the next BuildSearchBar.
    char searchText_[128] = {};
    bool focusSearch_ = false;
    // Typing debounce. A keystroke re-lists the whole folder, which on a CASC
    // root is not free, so the box is handed to the browser only once the user
    // has stopped typing: each keystroke re-arms this to kSearchDebounce and
    // NewFrame counts it down. 0 = the box and the browser agree.
    static constexpr float kSearchDebounce = 0.8f; // seconds
    float searchDelay_ = 0.0f;
    float iconSize_ = 128.0f;

    // Navigation staged by the UI, applied at the start of the next frame (see
    // NewFrame) so it can't invalidate the listing mid-iteration or destroy
    // thumbnails the current frame's draw data still references.
    bool navPending_ = false;
    bool navAscend_ = false;
    std::string navTarget_;

    // Open-transition timer: reset to 0 when the listing changes (folder
    // open/ascend, or OpenCasc) so the grid fades + slides in. Driven off
    // ImGui's frame DeltaTime in BuildGrid; 1.0 = settled (no animation).
    float navAnimT_ = 1.0f;
};

} // namespace whiteout::flakes::tools
