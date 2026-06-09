#pragma once

// ThumbnailPool — a capped pool of (Scene, offscreen RenderTarget) pairs, one
// per VISIBLE model/effect grid cell. Each frame the UI calls Acquire(path) for
// the cells currently on screen; the pool loads each file into its own scene,
// frames the camera, renders it into its target, and returns the target's color
// texture for ImGui::Image. Off-screen cells aren't acquired, so they cost
// nothing; when more cells are visible than the cap, the least-recently-visible
// live cell is recycled. This is the only file that depends on the multi-scene
// engine API (CreateScene / RenderViewport(scene) / GetTargetColorTexture).

#include "renderer/render_service.h"
#include "renderer/types.h"

#include "gfx/gfx.h"

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace whiteout::flakes::io {
class IContentProvider;
}

namespace whiteout::flakes::tools {

class ThumbnailPool {
public:
    ThumbnailPool(renderer::RenderService& svc,
                  std::shared_ptr<io::IContentProvider> provider, int cap = 12,
                  int resolution = 256);
    ~ThumbnailPool();

    ThumbnailPool(const ThumbnailPool&) = delete;
    ThumbnailPool& operator=(const ThumbnailPool&) = delete;

    // Start a frame: marks all live cells as not-yet-visible.
    void BeginFrame(std::uint64_t frameId);

    // Request the live thumbnail for `path` (an archive path). Marks the cell
    // visible this frame. Returns its color texture, or Invalid if the cell is
    // still loading / had to be deferred (the caller draws a placeholder).
    gfx::TextureHandle Acquire(const std::string& path, bool isEffect, std::uint64_t frameId);

    // Advance + render every cell marked visible this frame into its target.
    // Must run AFTER the UI built its draw list (so visibility is known) and
    // BEFORE the main window's ImGui pass samples the textures.
    void RenderVisible(float dt);

    // Tear down every live cell (on directory / archive change).
    void Clear();

    // Test hook: the RenderTarget of the first live cell (or 0). Lets a headless
    // test read back the same target.color ImGui samples.
    renderer::RenderTargetId DebugFirstCellTarget() const {
        return cells_.empty() ? 0 : cells_.front()->target;
    }

private:
    struct Cell {
        std::string path;
        renderer::SceneId scene = 0;
        renderer::RenderTargetId target = 0;
        u32 actor = 0; // actor handle in `scene`
        bool isEffect = false;
        bool isHd = false; // render this cell in HD vs SD
        bool loaded = false;
        bool visible = false;
        int effectWarmup = -1;     // >=0 counts up until the effect is framed
        float effectAge = 0.0f;    // seconds since this effect last (re)spawned
        std::uint64_t lastVisibleFrame = 0;
    };

    Cell* AcquireSlot(const std::string& path, bool isEffect, std::uint64_t frameId);
    void SetupScene(renderer::SceneId scene);
    void LoadCell(Cell& cell);
    void ResetEffect(Cell& cell);
    void DestroyCell(Cell& cell);

    renderer::RenderService& svc_;
    std::shared_ptr<io::IContentProvider> provider_;
    int cap_;
    int res_;
    std::vector<std::unique_ptr<Cell>> cells_;
    std::unordered_map<std::string, Cell*> byPath_;
};

} // namespace whiteout::flakes::tools
