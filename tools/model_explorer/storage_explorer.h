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

#include "casc_browser.h"
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

// Which file types the grid shows. Models = .mdx/.mdl, Effects = .pkb/.pkfx.
enum class StorageFileFilter { All, ModelsOnly, EffectsOnly };

// Concrete type of an activated file (a model dialect or an effect dialect).
enum class StorageFileKind { Mdx, Mdl, Pkb, Pkfx };

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
    bool IsOpen() const {
        return browser_.IsOpen();
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

    // ---- Per-frame host contract (call in this order) ----
    void NewFrame(float dt);
    void BuildWindow(bool* pOpen);
    void RenderThumbnails(float dt);

    // Archive path (CASC-native separators) of the last single-clicked file.
    const std::string& Selected() const {
        return selectedPath_;
    }

    // ---- File-type filter (consumer default + live UI combo) ----
    void SetFileFilter(StorageFileFilter f) {
        fileFilter_ = f;
    }
    StorageFileFilter FileFilter() const {
        return fileFilter_;
    }
    // Show the filter combo in the panel so the end-user can switch too
    // (default true). Hide it to lock the filter from SetFileFilter.
    void SetFilterUIVisible(bool on) {
        filterUiVisible_ = on;
    }

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
    void OpenCascDialog(); // native folder picker → OpenCasc

    renderer::RenderService& svc_;
    CascBrowser browser_;
    std::shared_ptr<io::FileContentProvider> provider_;
    std::unique_ptr<ThumbnailPool> pool_;

    std::uint64_t frameCounter_ = 0;
    std::string selectedPath_;
    std::string lastError_;
    ActivateCb onActivate_;
    StorageFileFilter fileFilter_ = StorageFileFilter::All;
    bool filterUiVisible_ = true;
    bool deliverBytes_ = false;

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
