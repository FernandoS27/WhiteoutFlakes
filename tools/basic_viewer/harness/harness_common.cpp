#include "harness/harness_common.h"

#include "renderer/render_pipeline.h"
#include "renderer/render_service.h"
#include "renderer/scene_manager.h"
#include "renderer/frame_ticker.h"
#include "renderer/model/model_loader.h"

#include <cstdio>
#include <cstdlib>
#include <iostream>

namespace whiteout::flakes::harness {

bool InitDevice(renderer::RenderPipeline& pipeline, gfx::GfxApi backend, std::string_view tag) {
    if (pipeline.InitDevice(backend))
        return true;
    std::cerr << "[" << tag << "] InitDevice failed" << std::endl;
    return false;
}

std::optional<Readback> ReadFrame(renderer::RenderPipeline& pipeline,
                                  renderer::RenderTargetId target, i32 width, i32 height) {
    const usize bytes = static_cast<usize>(width) * static_cast<usize>(height) * 4u;
    const auto fits = [&](const std::vector<u8>& rgba, i32 w, i32 h) {
        return w == width && h == height && rgba.size() >= bytes;
    };
    Readback frame;
    i32 w = 0;
    i32 h = 0;
    const i32 slot = pipeline.LastCapturedSlot();
    if (slot >= 0 && pipeline.DownloadCaptureSlot(slot, frame.rgba, w, h) && fits(frame.rgba, w, h))
        return frame;
    if (pipeline.ReadbackTarget(target, frame.rgba, w, h) && fits(frame.rgba, w, h)) {
        frame.viaTargetFallback = true;
        return frame;
    }
    return std::nullopt;
}

MeanRgb Mean(const std::vector<u8>& rgba, i32 pixels) {
    u64 r = 0, g = 0, b = 0;
    for (i32 p = 0; p < pixels; ++p) {
        r += rgba[static_cast<usize>(p) * 4 + 0];
        g += rgba[static_cast<usize>(p) * 4 + 1];
        b += rgba[static_cast<usize>(p) * 4 + 2];
    }
    return {static_cast<i32>(r / static_cast<u64>(pixels)),
            static_cast<i32>(g / static_cast<u64>(pixels)),
            static_cast<i32>(b / static_cast<u64>(pixels))};
}

Settle SettleAssets(renderer::RenderService& renderer, renderer::SceneManager& scene) {
    constexpr i32 kQuietIterations = 16;
    constexpr i32 kSettleCap = 2000;
    i32 quiet = 0;
    i32 iterations = 0;
    u64 lastActivity = ~0ull;
    for (; iterations < kSettleCap && quiet < kQuietIterations; ++iterations) {
        if (auto* cp = scene.ActiveContentProvider())
            cp->Pump();
        renderer.PumpAssetsViaProvider();
        renderer.Ticker().Tick(0.0f);
        renderer.Loader().CommitPendingUploads();
        const u64 activity = renderer.AssetActivityCounter();
        quiet = (activity == lastActivity) ? quiet + 1 : 0;
        lastActivity = activity;
    }
    return {quiet >= kQuietIterations, iterations};
}

void Exit(i32 code) {
    std::cout.flush();
    std::cerr.flush();
    std::fflush(stdout);
    std::fflush(stderr);
    std::_Exit(code);
}

} // namespace whiteout::flakes::harness
