#pragma once

// ============================================================================
// The gate harnesses: each brings a device up, drives a scene into an offscreen
// target with no window and no ViewerApp, prints `[tag]` lines a script reads,
// and ends the process with its verdict as the exit code.
//
// They _Exit rather than return from a finished run: RenderService and
// SceneManager destruct after the device is gone, which crashes, and a verdict
// already printed must not turn into a misleading exit status.
// ============================================================================

#include "cli/cli_options.h"
#include "whiteout/flakes/enums.h"
#include "whiteout/flakes/gfx_types.h"
#include "whiteout/flakes/types.h"

#include <filesystem>
#include <span>
#include <string>

namespace whiteout::flakes::renderer {
class RenderService;
class SceneManager;
} // namespace whiteout::flakes::renderer

namespace whiteout::flakes::harness {

struct GateContext {
    renderer::RenderService& renderer;
    renderer::SceneManager& scene;
    gfx::GfxApi backend;
    std::filesystem::path model;
    /// `--trace-frames`.
    i32 frames = 120;
    /// `--content-root`: the loose tree that resolves SNO- and path-addressed
    /// content above the model's own folder.
    std::string contentRoot;
};

/// `--headless-test`: offscreen render + readback of an empty scene or one
/// model; passes when the image is not black.
i32 RunHeadlessTest(const GateContext& ctx);

/// `--multiscene-test`: two scenes into two targets, the model in one only.
i32 RunMultiSceneTest(const GateContext& ctx);

/// `--childmodel-check`: PE1 Birth/Death balance, instance cap, bounded population.
i32 RunChildModelCheck(const GateContext& ctx);

/// `--particle-diff`: the L1/L2 particle trace harness.
i32 RunParticleDiff(const GateContext& ctx, const cli::ParticleDiffOptions& options);

/// `--draw-trace`: gates G1 (draw trace) and G2 (golden image).
/// @param game the install SNO/id lookups resolve through; Neutral for the
///        corpus arm.
i32 RunDrawTrace(const GateContext& ctx, const cli::DrawTraceOptions& options,
                 std::span<const std::filesystem::path> attachAnims,
                 ::whiteout::models::wem::ProfileId wemProfile, ProductId game);

} // namespace whiteout::flakes::harness
