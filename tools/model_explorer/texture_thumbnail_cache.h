#pragma once

// TextureThumbnailCache — ThumbnailPool's counterpart for images.
//
// A model cell is a scene: load it, frame a camera, render it into a target
// every frame. A texture cell is none of that. The file already IS the picture,
// so a cell here is one decode and one GPU upload, and after that the same
// handle is handed back every frame for free. That difference is why this is a
// separate class rather than a branch inside ThumbnailPool: there is no scene,
// no target, no per-frame render, and nothing in the pool's machinery that
// would carry over except the LRU.
//
// The host's contract is the same shape and the same order:
//
//   BeginFrame(frameId)  — mark every entry not-yet-visible
//   Acquire(path, …)     — per visible cell; returns Invalid while it waits
//   EndFrame()           — spend this frame's decode budget on what was asked
//
// Acquire returning Invalid is normal and self-correcting: only kDecodeBudget
// files are decoded per frame, so scrolling a screenful of icons fills in over
// the next few frames instead of stalling one. The caller draws a placeholder
// meanwhile, exactly as it does for a model cell that is still loading.

#include "renderer/render_service.h"

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

// What a decoded image turned out to be — for the caller's aspect-correct fit
// and for the preview pane's info line. Also the failure record: `ok` false
// with a `error` set is a file that will not be retried until the cache is
// cleared, which is what keeps a broken texture from being re-decoded every
// frame it is on screen.
struct TextureThumbnail {
    gfx::TextureHandle texture = gfx::TextureHandle::Invalid;
    int width = 0;
    int height = 0;
    bool hasAlpha = false;
    bool ok = false;
    std::string format; // source pixel format, e.g. "BLP1", "BC3" — for the UI
    std::string error;  // why it did not decode; empty when it did
};

class TextureThumbnailCache {
public:
    TextureThumbnailCache(renderer::RenderService& svc,
                          std::shared_ptr<io::IContentProvider> provider, int cap = 256);
    ~TextureThumbnailCache();

    TextureThumbnailCache(const TextureThumbnailCache&) = delete;
    TextureThumbnailCache& operator=(const TextureThumbnailCache&) = delete;

    void BeginFrame(std::uint64_t frameId);

    // Ask for @p archivePath's image and mark it visible this frame. Returns a
    // decoded entry, or one with `ok == false` and `error` empty while it is
    // still queued — the caller draws a placeholder for that. A decode that
    // FAILED comes back with `error` set, which a caller can show instead.
    const TextureThumbnail& Acquire(const std::string& archivePath, std::uint64_t frameId);

    // Decode up to kDecodeBudget of the files Acquire asked for this frame.
    // Runs after the UI has built its draw list, so the budget is spent on
    // what is actually on screen and in the order it was asked for.
    void EndFrame();

    // Drop every decoded image (on storage / folder change). Frees the GPU
    // textures, which is why it must run while the device is alive.
    void Clear();

    // Upper bound on decoded images kept. Past it the least-recently-visible
    // are released. Sized in ENTRIES rather than bytes because a thumbnail is
    // downsampled to a bounded edge before upload, so entries are the honest
    // unit.
    void SetCap(int cap);

private:
    struct Entry {
        TextureThumbnail info;
        std::uint64_t lastVisibleFrame = 0;
        // Frame this entry was last put on the decode queue, so one frame's
        // repeated Acquires (grid cell and preview pane can name the same
        // file) enqueue it once.
        std::uint64_t queuedFrame = 0;
        bool queued = true; // decoded entries clear it; a fresh one is waiting
    };

    void Decode(const std::string& archivePath, Entry& entry);
    void Trim();
    void Release(Entry& entry);

    // Files decoded per frame. A BLP is milliseconds, but a screenful of them
    // on the frame a folder opens is a visible hitch, and the grid fades in
    // over about that many frames anyway.
    static constexpr int kDecodeBudget = 6;
    // Longest edge a thumbnail is stored at. Terrain and UI sheets ship at
    // 1024² and larger; uploading those whole would spend a hundred megabytes
    // to draw a 128px cell.
    static constexpr int kMaxEdge = 256;

    renderer::RenderService& svc_;
    std::shared_ptr<io::IContentProvider> provider_;
    int cap_;
    std::unordered_map<std::string, Entry> entries_;
    std::vector<std::string> pending_; // this frame's Acquire misses, in order
    // Handed back from Acquire for an entry that has not been decoded yet, so
    // the caller always gets a reference it can read.
    TextureThumbnail placeholder_;
};

} // namespace whiteout::flakes::tools
