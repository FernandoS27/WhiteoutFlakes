#pragma once

// ============================================================================
// The Storage Explorer panel embedded in the viewer (Tools ▸ Storage Explorer):
// created on first show, fed each product's install keys, persisted to the
// `[StorageExplorer]` ini section once its state settles.
// ============================================================================

#include "whiteout/flakes/types.h"

#include <functional>
#include <memory>
#include <string>

namespace whiteout::flakes::io {
class IContentProvider;
class LoadTaskRunner;
} // namespace whiteout::flakes::io
namespace whiteout::flakes::renderer {
class RenderService;
}
namespace whiteout::flakes::tools {
class StorageExplorer;
struct ActivatedFile;
} // namespace whiteout::flakes::tools

namespace whiteout::flakes {

class StorageSession;

class StorageExplorerHost {
public:
    using OnActivate = std::function<void(const tools::ActivatedFile&)>;

    StorageExplorerHost(renderer::RenderService& service, io::LoadTaskRunner& tasks,
                        StorageSession& session, OnActivate onActivate);
    ~StorageExplorerHost();

    bool IsOpen() const {
        return open_;
    }
    /// Showing re-syncs the panel with what the host knows every time, not only
    /// the first: a listfile adopted beside a model opened after the panel was
    /// built would otherwise never reach it. Sync reopens only when something
    /// moved, so the folder being browsed survives a close and reopen.
    void SetOpen(bool on);
    /// Destroy the panel (its thumbnail scenes and offscreen targets) while the
    /// device that owns them is still alive.
    void Shutdown();

    /// Before the UI frame: pump the panel's own provider, apply staged
    /// navigation, and mark its thumbnail cells not yet visible.
    void NewFrame(f32 dt);
    /// Inside the UI frame.
    void BuildWindow();
    /// After the UI frame, before the main pass composites the draw data that
    /// samples them. Juggles the active scene per cell; the caller re-publishes.
    /// False when there was nothing to render.
    bool RenderThumbnails(f32 dt);

private:
    /// Save the panel's ini section once its state stops changing — only while
    /// it is open AND settled: a panel mid-open describes the storage it is
    /// leaving, and saving that would hand the next session the wrong game.
    void PollState(f32 dt);

    renderer::RenderService& service_;
    io::LoadTaskRunner& tasks_;
    StorageSession& session_;
    OnActivate onActivate_;
    std::unique_ptr<tools::StorageExplorer> explorer_;
    bool open_ = false;
    /// What the ini section last held, and how long until it is rewritten. The
    /// state moves continuously while a splitter or a zoom slider is dragged, so
    /// the write waits for it to settle.
    std::string stateKey_;
    f32 saveDelay_ = 0.0f;
};

} // namespace whiteout::flakes
