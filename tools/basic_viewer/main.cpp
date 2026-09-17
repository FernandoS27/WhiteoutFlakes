// The viewer's entry point: capture the log, parse the command line, settle the
// platform, then either hand over to a gate harness (no window, no app) or run
// the viewer for whatever mode the options select.

#include "backend_names.h"
#include "cli/cli_commands.h"
#include "cli/cli_options.h"
#include "gfx/gfx.h"
#include "harness/harness.h"
#include "io/file_content_provider.h"
#include "log_console.h"
#include "platform/platform_paths.h"
#include "renderer/render_service.h"
#include "renderer/scene_manager.h"
#include "settings_ini.h"
#include "whiteout/flakes/util/path_utf8.h"

#include <iostream>

namespace wf = whiteout::flakes;

int main(int argc, char* argv[]) {
    // Before anything logs, so startup output reaches the in-app Log Console:
    // in Release the exe has no console window, and this is the only dev log.
    wf::tools::LogConsole::Instance().Begin();

    const wf::cli::ParseResult parsed = wf::cli::Parse(wf::platform::Utf8Arguments(argc, argv));
    std::cerr << parsed.warnings;
    if (parsed.exitCode) {
        (parsed.messageIsError ? std::cerr : std::cout) << parsed.message;
        return *parsed.exitCode;
    }
    const wf::cli::CliOptions& options = parsed.options;
    const wf::cli::RunMode mode = wf::cli::SelectRunMode(options);
    if (!options.wgpuBackend.empty())
        wf::gfx::SetWebGPUBackend(options.wgpuBackend.c_str());

    wf::platform::ConfigureVulkanEnvironment();
    wf::renderer::SceneManager scene;
    wf::renderer::RenderService renderer(scene);

    // The gates script render modes explicitly per scenario and every golden was
    // recorded against exactly that: the loader must not true the scene up to
    // whatever the parsed template prefers.
    renderer.Settings().SetFollowModelRenderMode(false);

    // Startup-only settings (validation layer, default backend, preferred
    // device) must land before gfx::CreateDevice runs.
    wf::LoadStartupSettingsFromIni(renderer);
    const wf::gfx::GfxApi backend = wf::platform::CoerceBackendToPlatform(
        options.backend.value_or(renderer.Settings().DefaultBackend()), renderer.Settings());
    wf::platform::ConfigurePipelineCache();
    std::cout << "Backend: " << wf::tools::BackendNameOf(backend).label << "\n";

    // World of Warcraft's slot specifically, and left there: it is that
    // product's config, and the run does not know yet which game the model
    // belongs to. Restoring the selection keeps this from being a back-door SetGame.
    if (!options.listfilePath.empty() || !options.tactKeyPath.empty()) {
        auto& cp = renderer.Scene().GetContentProvider();
        const auto was = cp.Game();
        cp.SetGame(wf::ProductId::Wow);
        if (!options.listfilePath.empty())
            cp.SetListfilePath(wf::io::FsPathFromUtf8(options.listfilePath));
        if (!options.tactKeyPath.empty())
            cp.SetTactKeyPath(wf::io::FsPathFromUtf8(options.tactKeyPath));
        cp.SetGame(was);
    }

    // An explicit opt-in, never inferred from the extension: Diablo III reaches
    // its ShaderMap, Shaders and shared Materials by SNO id, which resolve only
    // through an opened storage. The corpus arm deliberately runs without it.
    auto traceGame = wf::ProductId::Neutral;
    if (const auto& name = options.drawTraceOptions.game; !name.empty()) {
        const auto game = wf::cli::TraceGameFromName(name);
        if (!game) {
            std::cerr << "[dtrace] --draw-trace-game: expected wc3|wow|sc2|d3, got '" << name << "'"
                      << std::endl;
            return 2;
        }
        traceGame = *game;
    }

    const wf::harness::GateContext gate{renderer, scene, backend, options.model,
                                        options.traceFrames, options.contentRoot};
    switch (mode) {
    case wf::cli::RunMode::HeadlessTest:
        return wf::harness::RunHeadlessTest(gate);
    case wf::cli::RunMode::MultiSceneTest:
        return wf::harness::RunMultiSceneTest(gate);
    case wf::cli::RunMode::ChildModelCheck:
        return wf::harness::RunChildModelCheck(gate);
    case wf::cli::RunMode::ParticleDiff:
        return wf::harness::RunParticleDiff(gate, options.particleDiffOptions);
    default:
        break;
    }

    // Applied here and not inside the trace, as they always were: a scripted
    // viewer run reaches them too, and nothing between here and the trace
    // reloads the settings block.
    const auto& trace = options.drawTraceOptions;
    if (trace.noClothDeform)
        renderer.Settings().SetClothDeform(false);
    if (trace.sdHdr)
        renderer.Settings().SetSceneHdrInSd(true);
    if (trace.debugView >= 0)
        renderer.Settings().SetHdDebugMode(trace.debugView);
    if (mode == wf::cli::RunMode::DrawTrace)
        return wf::harness::RunDrawTrace(gate, trace, options.attachAnims, options.wemProfile, traceGame);

    return wf::cli::RunViewer(renderer, scene, options, mode, backend);
}
