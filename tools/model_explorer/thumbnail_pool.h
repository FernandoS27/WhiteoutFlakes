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

    // Which game the cells are about to show. Only used to decide whether a
    // new cell scene stands the Warcraft III lighting up (see SetupScene) —
    // the models themselves are read by content, not by this.
    void SetProduct(ProductId product) {
        product_ = product;
    }

    // Resize the live-cell budget. A cell that finds no free slot renders
    // nothing, so the cap has to cover a whole screen of cells — and how many
    // that is depends on the grid's icon size, which the user drives. Shrinking
    // destroys the least-recently-visible cells above the new cap; they reload
    // if they come back on screen.
    void SetCap(int cap);
    int Cap() const {
        return cap_;
    }

    // Request the live thumbnail for `path` (an archive path). Marks the cell
    // visible this frame. Returns its color texture, or Invalid if the cell is
    // still loading / had to be deferred (the caller draws a placeholder).
    // `pixelSize` is the size (in framebuffer pixels) the caller will DISPLAY the
    // thumbnail at; the cell's render target is sized to match (rounded/clamped)
    // so a large grid cell stays crisp instead of upscaling a small texture.
    // 0 keeps the pool's default resolution.
    gfx::TextureHandle Acquire(const std::string& path, bool isEffect, std::uint64_t frameId,
                               int pixelSize = 0);

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
    // Test hook: its scene. A cell's lighting is per scene, so this is what a
    // gate makes active before asking RenderService::GetDncService().
    renderer::SceneId DebugFirstCellScene() const {
        return cells_.empty() ? 0 : cells_.front()->scene;
    }

private:
    struct Cell {
        std::string path;
        renderer::SceneId scene = 0;
        renderer::RenderTargetId target = 0;
        int res = 0; // current target edge length in pixels
        // Colour format the target was built with. Which one a cell needs is a
        // property of the frame that draws it — see EnsureCellTargetFormat.
        gfx::Format fmt = gfx::Format::R8G8B8A8_UNORM;
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
    // Size `cell`'s render target to `wantPx` (rounded to a step + clamped to
    // [res_, kMaxRes]) so it matches the on-screen cell size. Recreates the
    // target (with a GPU drain) only when the rounded size actually changes.
    void EnsureCellTargetSize(Cell& cell, int wantPx);
    // Rebuild `cell`'s target if the frame about to draw it composites in the
    // other colour space. Call with the cell's scene active and its settings
    // applied — that is what the answer depends on.
    void EnsureCellTargetFormat(Cell& cell);
    void SetupScene(renderer::SceneId scene);
    void LoadCell(Cell& cell);
    void ResetEffect(Cell& cell);
    void DestroyCell(Cell& cell);

    renderer::RenderService& svc_;
    std::shared_ptr<io::IContentProvider> provider_;
    ProductId product_ = ProductId::Neutral;
    int cap_;
    int res_;
    std::vector<std::unique_ptr<Cell>> cells_;
    std::unordered_map<std::string, Cell*> byPath_;
};

} // namespace whiteout::flakes::tools
