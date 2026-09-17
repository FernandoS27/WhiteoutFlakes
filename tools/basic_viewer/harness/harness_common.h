#pragma once

// ============================================================================
// What the gate harnesses share: fixed steps and target sizes, exit codes,
// device bring-up, the readback, the asset settle loop and the exit itself.
// ============================================================================

#include "render_target.h"
#include "whiteout/flakes/gfx_types.h"
#include "whiteout/flakes/types.h"

#include <optional>
#include <string_view>
#include <vector>

namespace whiteout::flakes::renderer {
class RenderPipeline;
class RenderService;
class SceneManager;
} // namespace whiteout::flakes::renderer

namespace whiteout::flakes::harness {

/// The trace gates' step: a frame number is an exact 60 Hz millisecond.
inline constexpr f32 kTraceStep = 1.0f / 60.0f;
/// The smoke tests' step.
inline constexpr f32 kSmokeStep = 0.016f;

inline constexpr i32 kSmokeTargetSize = 256;
/// Also the size of every G2 golden: changing it re-baselines them all.
inline constexpr i32 kTraceTargetSize = 512;

/// Exit codes. Scripts test for zero; the rest say which stage failed.
namespace exit_code {
inline constexpr i32 kPass = 0;
inline constexpr i32 kUsage = 2;
inline constexpr i32 kDevice = 3;
/// The two smoke tests number their setup stages from 2 instead.
inline constexpr i32 kSmokeDevice = 2;
inline constexpr i32 kSmokeTarget = 3;
inline constexpr i32 kSpawn = 4;
/// A trace file could not be read or written, or assets never settled.
inline constexpr i32 kTraceIo = 5;
/// An asset arrived mid-capture, so the trace would be timing-dependent.
inline constexpr i32 kLateAsset = 6;
// Each harness's own "ran, and failed" verdict.
inline constexpr i32 kHeadlessFail = 6;
inline constexpr i32 kMultiSceneFail = 7;
inline constexpr i32 kParticleDiffFail = 8;
inline constexpr i32 kDrawTraceFail = 9;
inline constexpr i32 kChildModelFail = 10;
} // namespace exit_code

/// InitDevice, logging `[tag] InitDevice failed` when it does not come up.
bool InitDevice(renderer::RenderPipeline& pipeline, gfx::GfxApi backend, std::string_view tag);

/// One frame's pixels: the capture ring's last slot, or a direct target
/// readback on a backend without compute capture (WebGPU).
struct Readback {
    std::vector<u8> rgba;
    bool viaTargetFallback = false;
};
std::optional<Readback> ReadFrame(renderer::RenderPipeline& pipeline,
                                  renderer::RenderTargetId target, i32 width, i32 height);

struct MeanRgb {
    i32 r = 0;
    i32 g = 0;
    i32 b = 0;
    bool Black() const {
        return r == 0 && g == 0 && b == 0;
    }
};
MeanRgb Mean(const std::vector<u8>& rgba, i32 pixels);

/// Drain asset arrival: pump until the activity counter has held still for a
/// run of iterations, or give up at a cap.
struct Settle {
    bool settled = false;
    i32 iterations = 0;
};
Settle SettleAssets(renderer::RenderService& renderer, renderer::SceneManager& scene);

/// Flush and end the process now, past the static teardown that crashes once
/// the device is gone.
[[noreturn]] void Exit(i32 code);

} // namespace whiteout::flakes::harness
