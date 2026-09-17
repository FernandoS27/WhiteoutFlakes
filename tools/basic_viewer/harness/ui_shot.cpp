#include "harness/ui_shot.h"

#include "io/load_task.h"
#include "renderer/render_pipeline.h"
#include "renderer/render_service.h"
#include "app/viewer_app.h"
#include "ui/viewer_ui.h"
#include "whiteout/flakes/util/path_utf8.h"

#include "gfx/gfx.h"

#include <whiteout/textures/png/writer.h>
#include <whiteout/textures/texture.h>

#include <imgui.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>

namespace whiteout::flakes {

namespace {

// Consecutive frames with no asset activity and no running task before the
// capture. ImGui's auto-resizing popups need a few frames of their own too.
constexpr i32 kQuietFrames = 30;
constexpr auto kSettleTimeout = std::chrono::seconds(180);

} // namespace

i32 RunUiShot(ViewerApp& app, const UiShotOptions& options) {
    // A saved layout in the working directory would move every window.
    ImGui::GetIO().IniFilename = nullptr;

    if (!app.Ui().OpenPanelForShot(options.panel)) {
        std::fprintf(stderr, "[uishot] panel '%s' is unknown or not available for this model\n",
                     options.panel.c_str());
        return 2;
    }
    // The UI is what is being captured. The document still drives every panel;
    // only its pixels are left out.
    app.Playback().SetPaused(true);
    app.SetSceneHiddenForCapture(true);

    auto& service = app.Service();
    auto& pipeline = service.Pipeline();
    const auto deadline = std::chrono::steady_clock::now() + kSettleTimeout;
    u64 lastActivity = ~0ull;
    i32 quiet = 0;
    i32 frames = 0;
    while (quiet < kQuietFrames && std::chrono::steady_clock::now() < deadline) {
        app.Tick(0.0f);
        const u64 activity = service.AssetActivityCounter();
        quiet = (activity == lastActivity && !app.Tasks().Busy()) ? quiet + 1 : 0;
        lastActivity = activity;
        ++frames;
    }
    if (quiet < kQuietFrames) {
        std::fprintf(stderr, "[uishot] never settled after %d frame(s)\n", frames);
        return 3;
    }

    pipeline.EnableFrameCapture(true);
    app.Tick(0.0f);
    if (auto* dev = pipeline.Gfx())
        dev->WaitIdle();
    std::vector<u8> rgba;
    i32 w = 0;
    i32 h = 0;
    // Download before disabling: turning capture off frees the ring.
    const i32 slot = pipeline.LastCapturedSlot();
    const bool captured = slot >= 0 && pipeline.DownloadCaptureSlot(slot, rgba, w, h);
    pipeline.EnableFrameCapture(false);
    if (!captured || w <= 0 || h <= 0 ||
        rgba.size() < static_cast<usize>(w) * static_cast<usize>(h) * 4u) {
        std::fprintf(stderr, "[uishot] frame capture failed\n");
        return 3;
    }

    namespace tx = whiteout::textures;
    auto image =
        tx::Texture::create2D(tx::PixelFormat::RGBA8, static_cast<u32>(w), static_cast<u32>(h), 1);
    auto pixels = image.mipData(0);
    std::memcpy(pixels.data(), rgba.data(), pixels.size());
    // The capture's alpha is the scene target's, not coverage.
    for (usize a = 3; a < pixels.size(); a += 4)
        pixels[a] = 0xFF;
    tx::png::Writer writer;
    const std::vector<u8> png = writer.write(image);
    std::error_code ec;
    if (!options.out.parent_path().empty())
        std::filesystem::create_directories(options.out.parent_path(), ec);
    std::ofstream file(options.out, std::ios::binary);
    if (png.empty() || !file ||
        !file.write(reinterpret_cast<const char*>(png.data()),
                    static_cast<std::streamsize>(png.size()))) {
        std::fprintf(stderr, "[uishot] cannot write %s\n", io::PathToUtf8(options.out).c_str());
        return 3;
    }
    std::printf("[uishot] %s: %dx%d after %d frame(s) -> %s\n", options.panel.c_str(), w, h,
                frames, io::PathToUtf8(options.out).c_str());
    return 0;
}

} // namespace whiteout::flakes
