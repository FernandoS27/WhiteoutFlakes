#pragma once

// StorageExplorer — the embeddable model/storage explorer panel. Browses a CASC
// archive and renders live model/effect thumbnails, each in its own
// RenderService scene + offscreen viewport, as either a grid of icons or a
// tree beside one large preview (ExplorerView). Owns NO window / gfx device / ImGui
// context: a host supplies all three plus a shared RenderService, then drives
// the per-frame contract each frame, in order:
//
//   NewFrame(dt)         — pump the explorer's provider + apply staged nav
//   BuildWindow(&open)   — build the panel window (emits ImGui::Image cmds)
//   RenderThumbnails(dt) — render visible cells into their targets (settings
//                          guarded so the host's main view is untouched)
//
// See STORAGE_EXPLORER_DESIGN.md.

#include "explorer_state.h" // ExplorerView, ExplorerState
#include "io/storage_browser.h"

namespace whiteout::flakes::io {
class LoadTaskRunner;
}
#include "texture_thumbnail_cache.h"
#include "thumbnail_pool.h"

#include "renderer/render_service.h"
#include "renderer/types.h"
#include "whiteout/flakes/gfx_types.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace whiteout::flakes::io {
class FileContentProvider;
class IContentProvider;
} // namespace whiteout::flakes::io

namespace whiteout::flakes::tools {

// Concrete type of an activated file (a model dialect, an effect dialect, or
// an image). Texture is the one kind that is not drawn as a scene: it has no
// actor, no camera and no animation — the file is already the picture.
enum class StorageFileKind { Mdx, Mdl, Pkb, Pkfx, M2, M3, Texture };

// Handed to the activate callback when a file is double-clicked. `path` (the
// CASC-native archive path) and `provider` (the storage it lives in) are always
// set; `bytes` is filled only when DeliverBytes() is on — the panel reads the
// file synchronously and hands the consumer the content directly.
struct ActivatedFile {
    std::string path;
    StorageFileKind kind = StorageFileKind::Mdx;
    bool isEffect = false;  // kind is Pkb/Pkfx
    bool isTexture = false; // kind is Texture
    std::shared_ptr<io::IContentProvider> provider;
    bool hasBytes = false;
    std::vector<std::uint8_t> bytes;
};

// Everything a host knows about where one product's storage lives: its install
// override, plus the listfile and TACT key list. An empty `installPath` means
// "whatever this machine has detected".
// One storage a host already knows about, offered by name in the File menu.
// A host that has resolved its installs itself — out of its own settings,
// where a game combo driven by BlizzardGameFinder would not look — lists them
// here rather than making the user re-find them with the folder picker. The
// order is the host's; the first is what it opened.
struct NamedRoot {
    std::string label; // "Warcraft III Reforged"
    std::string root;  // the install directory
};

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

    // Open @p root as whatever it actually is (io::StorageBrowser::OpenAuto's
    // rule): a CASC install, a directory of .mpq archives, a single archive,
    // or a loose folder.
    //
    // This is what a host should call when it was handed "the Warcraft III
    // install" and cannot know which generation it got — a Reforged install is
    // CASC, a 1.2x one is four MPQs in a directory, and OpenCasc on the latter
    // fails with a message about a missing .build.info that tells the user
    // nothing they can act on.
    bool OpenStorage(const std::string& root);

    // Storages to list by name at the top of the File menu, each opened with
    // OpenStorage. Purely additive: the folder picker stays below them, and a
    // host that sets none gets the menu it had. The entry matching the open
    // root is check-marked, so the menu also answers "which one am I in?".
    void SetNamedRoots(std::vector<NamedRoot> roots) {
        namedRoots_ = std::move(roots);
    }
    const std::vector<NamedRoot>& NamedRoots() const {
        return namedRoots_;
    }

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

    // Test hook: the scene of the first live thumbnail cell (0 if none). The
    // day/night rig is per scene, so a gate has to make this one active before
    // it can ask whether a preview is lit.
    renderer::SceneId DebugFirstCellScene() const;

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
    // Narrow what the next open WALKS for, not merely what it lists — see
    // io::StorageBrowser::SetOpenTypes. A picker that shows one kind should
    // set this to that kind before opening; SetBrowseTypes alone still pays
    // for the tree it then hides.
    void SetOpenTypes(io::BrowseType types) {
        browser_.SetOpenTypes(types);
    }
    io::BrowseType BrowseTypes() const {
        return browser_.EnabledTypes();
    }
    // Show the game combo + type checkboxes in the panel so the end-user can
    // switch too (default true). Hide it to lock what SetBrowseTypes chose. The
    // search box, the zoom and the view switch are part of the browser itself
    // and stay either way.
    void SetFilterUIVisible(bool on) {
        filterUiVisible_ = on;
    }

    // Which browser the panel shows. The user drives this with the switch in
    // the panel's top-right corner; a host can preselect one. Switching to the
    // tree reveals the folder the grid was in, so the two feel like one
    // browser rather than two places.
    void SetView(ExplorerView view);
    ExplorerView View() const {
        return view_;
    }

    // ---- Session state (see ExplorerState) ----
    //
    // A host that wants the panel to come back as it was reads State() while
    // the panel is open and settled (IsOpen() && !Opening(); anything else
    // describes a panel that is between storages) and hands it to RestoreState
    // once, right after construction and BEFORE Sync - which prefers the
    // restored game over its own fallback.
    ExplorerState State() const;
    void RestoreState(const ExplorerState& state);

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
    // The one open path. `kind` unset means "work out what @p root is"
    // (io::StorageBrowser::OpenAuto); set pins it, which is what OpenCasc and
    // Sync's reopen want.
    bool OpenInternal(const std::string& root, std::optional<io::StorageKind> kind);
    void BuildGrid();      // breadcrumb + folder/file grid (inside the open window)
    void BuildTree();      // outline + preview panes (inside the open window)
    void BuildTreePane(float width, float height); // the outline half
    void BuildPreviewPane();                       // the one-thumbnail half
    void BuildViewSwitch(); // the Grid/Tree control in the menu bar
    void BuildFilterBar(); // game combo + one checkbox per browsable type
    void BuildSearchBar(); // search box + match count + zoom
    void BuildEmptyHint(); // why an opened storage enumerated nothing
    // Hand a file to the host's activate callback ("open this"). Shared by a
    // grid cell's double-click and a tree row's.
    void Activate(const std::string& archivePath, StorageFileKind kind);
    void ClearSelection();
    // The staged half of RestoreState, run by the open that completes.
    void ApplyStagedRestore();
    // Flatten the OPEN subtrees into treeRows_. Only what an expanded folder
    // exposes is walked, so the cost tracks what is reachable on screen and
    // not what the storage holds.
    void RebuildTreeRows();
    void AppendTreeRows(const std::string& displayPath, int depth, bool autoExpand);
    // Open every ancestor of @p displayPath, and scroll the outline to it.
    void RevealFolder(const std::string& displayPath);
    // What treeRows_ was built for: storage, filter, type mask. Anything else
    // that reshapes the outline (a folder toggled open) sets the dirty flag
    // directly.
    std::string TreeSignature() const;
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
    // Textures do not go through the pool: they are decoded once and sampled,
    // not loaded into a scene and rendered. See texture_thumbnail_cache.h.
    std::unique_ptr<TextureThumbnailCache> textures_;
    // What the last successful open turned out to be, so a reopen driven by
    // Sync goes back to the same kind rather than re-guessing — and, more to
    // the point, does not reopen a classic MPQ install as CASC.
    io::StorageKind openedKind_ = io::StorageKind::Casc;

    std::uint64_t frameCounter_ = 0;
    std::string selectedPath_;
    std::string lastError_;
    ActivateCb onActivate_;
    // Kept so a game switch can re-apply them: they belong to the provider's
    // per-product slot, not to one open.
    std::string listfilePath_;
    std::string tactKeyPath_;
    GameKeysCb gameKeys_;
    std::vector<NamedRoot> namedRoots_;
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

    // ---- Staged restore (see ExplorerState / RestoreState) ----
    // Applied by the first open that completes, and dropped either way: a
    // folder from last session belongs to the storage it was recorded in, so a
    // user who opens a different game first must not land in it.
    bool restorePending_ = false;
    ProductId restoreGame_ = ProductId::Neutral;
    io::BrowseType restoreTypes_ = io::BrowseType::None;
    std::string restoreFolder_;
    std::string restoreFilter_;
    std::string restoreSelected_;

    // ---- Tree view ----
    ExplorerView view_ = ExplorerView::Grid;

    // One row of the outline, folder or file, already positioned by `depth`.
    struct TreeRow {
        std::string name;    // display name (the label)
        std::string path;    // full display path, and the row's identity
        std::string archive; // what the provider reads (files only)
        int depth = 0;
        bool isFolder = false;
        bool open = false; // folders: expanded this rebuild
    };
    // Flattened rows, rebuilt only when the outline's shape moves (see
    // TreeSignature) and drawn through an ImGuiListClipper. Never per frame: a
    // World of Warcraft tree is tens of thousands of nodes, and emitting even
    // the labels for one open folder of `creature/` overruns ImGui's 16-bit
    // index buffer - the same wall the grid culls off-screen cells for.
    std::vector<TreeRow> treeRows_;
    bool treeRowsDirty_ = true;
    bool treeTruncated_ = false; // rows hit kMaxTreeRows; say so rather than lie
    // The filter is narrow enough that it, and not the user, decides what is
    // expanded. See RebuildTreeRows.
    bool treeAutoExpand_ = false;
    std::string treeSig_;
    // Folders the user opened, by lowercase display path.
    std::set<std::string> treeOpen_;
    // Display path of the selected row (selectedPath_ is its archive path) and
    // the kind the preview needs to know to render it as an effect.
    std::string treeSelectedDisplay_;
    StorageFileKind selectedKind_ = StorageFileKind::Mdx;
    // Set by RevealFolder, consumed by the next BuildTreePane: a reveal nobody
    // scrolled to is a reveal that did not happen.
    std::string treeScrollTo_;
    // Outline pane width in logical pixels; the splitter between the panes
    // drags it.
    float treeSplit_ = 300.0f;
};

} // namespace whiteout::flakes::tools
