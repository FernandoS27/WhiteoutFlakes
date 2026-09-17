#include "cli/cli_commands.h"

#include "app/viewer_tuning.h"
#include "string_util.h"
#include "cubeb_sound_emitter.h"
#include "capture/export_ini.h"
#include "capture/export_recipe.h"
#include "harness/ui_shot.h"
#include "localization.h"
#include "renderer/render_service.h"
#include "renderer/scene_manager.h"
#include "settings_ini.h"
#include "ui/file_dialogs.h"
#include "ui/ui_metrics.h"
#include "app/viewer_app.h"
#include "whiteout/flakes/util/path_utf8.h"

#include <nfd.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>

namespace whiteout::flakes::cli {

namespace {

namespace fs = std::filesystem;

// The scripted modes that work on a loaded model, and the flag that asked.
const char* ModelFlagOf(RunMode mode) {
    switch (mode) {
    case RunMode::ListClips:
        return "--list-clips";
    case RunMode::ExportWem:
        return "--export-wem";
    case RunMode::ExportMdx:
        return "--export-mdx";
    case RunMode::ExportM3:
        return "--export-m3";
    case RunMode::ExportGltf:
        return "--export-gltf";
    case RunMode::SaveM3:
        return "--save-m3";
    case RunMode::CaptureAnimation:
        return "--export-anim";
    default:
        return nullptr;
    }
}

// Persistent settings, the UI language and the IO profile. After Open: they
// validate against services the device brings up (ShadowService's cascades).
void ApplySettings(ViewerApp& app, renderer::RenderService& renderer, const CliOptions& options) {
    bool loopPolicy = app.Playback().LoopNonLooping();
    bool forceHdPolicy = app.Loader().ForceHd();
    std::string languageCode = "en";
    LoadSettingsIni(renderer, loopPolicy, forceHdPolicy, languageCode);
    app.Playback().SetLoopNonLooping(loopPolicy);
    app.Loader().SetForceHd(forceHdPolicy);

    // The `<code>.ini` catalogs in `lang/` next to the exe; English otherwise.
    i18n::Localizer::instance().load(io::PathToUtf8(AssetDir() / "lang"),
                                     i18n::languageFromCode(languageCode));

    // Settings > IO, per game: the profile the panel was left on is loaded now.
    // Through the app so the product is recorded as configured — switching onto
    // it later must not re-apply (see ViewerApp::ApplyProfile).
    app.Session().ApplyProfile(app.Session().SettingsProfile(), /*force=*/true);

    // After the profile, because loading settings overwrites the flag block.
    if (options.showCollisions) {
        auto df = renderer.Settings().GetDisplayFlags();
        df.showCollisions = true;
        renderer.Settings().SetDisplayFlags(df);
    }
}

// The documents the command line names. A scripted run loads them now: its
// work follows immediately and cannot wait for a load that finishes later.
// The interactive viewer queues them for the frame loop, which puts a progress
// bar in front of each — loading here, before any frame exists, is what froze
// the window with nothing on screen.
void OpenDocuments(ViewerApp& app, const CliOptions& options, bool loadNow) {
    // Before any open: the profile decides which game's storage the textures
    // resolve against, which FollowModelGame settles on the way in.
    app.Loader().SetPreferredWemProfile(options.wemProfile);

    if (!options.model.empty()) {
        // No exists() pre-check for a synchronous load: LoadModel accepts a
        // storage-internal path and says so itself when nothing resolves it.
        if (loadNow) {
            if (!app.Loader().LoadModel(options.model))
                std::cerr << "Failed to load model.\n";
        } else if (!fs::exists(options.model)) {
            std::cerr << "File not found: " << io::PathToUtf8(options.model) << "\n";
        } else {
            app.Loader().QueueInitialOpen(options.model);
        }
    }
    // The rest in their own tabs; the last one loaded ends up active.
    for (const auto& extra : options.extraModels) {
        if (!fs::exists(extra)) {
            std::cerr << "File not found: " << io::PathToUtf8(extra) << "\n";
        } else if (loadNow) {
            if (!app.Loader().LoadModel(extra))
                std::cerr << "Failed to load model: " << io::PathToUtf8(extra) << "\n";
        } else {
            app.Loader().QueueInitialOpen(extra);
        }
    }

    // After this the sequence list — the toolbar dropdown and --export-anim's
    // index — spans the model's sequences followed by each attached file's.
    for (const auto& anim : options.attachAnims) {
        Sc2AnimationFiles* sc2 = app.Features().Sc2();
        if (!sc2 || !sc2->Attach(anim)) {
            std::cerr << "Failed to attach animation file: " << io::PathToUtf8(anim) << "\n";
        } else {
            std::printf("[viewer] attached '%s'; %zu sequence(s) now available\n",
                        io::PathToUtf8(anim.filename()).c_str(), app.Playback().SequenceNames().size());
        }
    }
}

// What the model can play, and where each clip came from: the gate for the
// catalog attach, since a Heroes hero carries a handful of its own sequences
// and gets the rest from the `.m3a` its `CModel` entry names.
i32 ListClips(ViewerApp& app) {
    const auto& names = app.Playback().SequenceNames();
    const Sc2AnimationFiles* sc2 = app.Features().Sc2();
    const auto attached = sc2 ? sc2->AttachedFiles() : std::vector<Sc2AnimationFiles::Attached>{};
    usize fromFiles = 0;
    for (const auto& a : attached)
        fromFiles += a.sequenceCount;
    std::printf("[clips] %zu sequence(s): %zu from the model, %zu from %zu attached file(s)\n",
                names.size(), names.size() - (std::min)(names.size(), fromFiles), fromFiles,
                attached.size());
    for (const auto& a : attached)
        std::printf("[clips]   attached '%s': %zu sequence(s) at %zu\n", a.label.c_str(),
                    a.sequenceCount, a.firstSequence);
    for (usize i = 0; i < names.size(); ++i)
        std::printf("[clips]   [%zu] %s\n", i, names[i].c_str());
    return names.empty() ? 1 : 0;
}

// The conversions read the parsed model the load produced and the content
// provider: nothing on the GPU, so no warm-up ticks.
bool Convert(ViewerApp& app, const ConvertOptions& c, RunMode mode) {
    const auto logged = [](std::string_view operation, const ExportOutcome& outcome) {
        LogOutcome(operation, outcome);
        return outcome.ok;
    };
    ModelExportService& exports = app.Exports();
    switch (mode) {
    case RunMode::ExportWem:
        return logged("Export WEM", exports.ExportWem(c.wemPath));
    case RunMode::ExportMdx:
        return logged("Export MDX", exports.ExportMdx(c.mdxPath, c.mdx));
    case RunMode::ExportM3:
        return logged("Export M3", exports.ExportM3(c.m3Path, c.m3));
    case RunMode::ExportGltf: {
        GltfExportOptions gltf = c.gltf;
        gltf.binary = tools::ToLowerAscii(io::PathToUtf8(c.gltfPath.extension())) != ".gltf";
        return logged("Export glTF", exports.ExportGltf(c.gltfPath, gltf));
    }
    case RunMode::SaveM3:
        // After the attaches on purpose: --save-m3-merge-anims folds them in.
        return logged("Save M3", exports.SaveM3(c.saveM3Path, c.saveM3));
    default:
        return false;
    }
}

// `name|#index[:repeats][@speed]`.
ExportClip ParseClipSpec(std::string spec, std::span<const std::string> sequenceNames) {
    ExportClip clip;
    if (const usize at = spec.rfind('@'); at != std::string::npos) {
        clip.speed = static_cast<f32>(std::atof(spec.c_str() + at + 1));
        spec.resize(at);
    }
    if (const usize colon = spec.rfind(':'); colon != std::string::npos) {
        clip.repeats = std::atoi(spec.c_str() + colon + 1);
        spec.resize(colon);
    }
    if (!spec.empty() && spec[0] == '#') {
        clip.sequence = std::atoi(spec.c_str() + 1);
        clip.savedName = SequenceKey(sequenceNames, clip.sequence);
    } else {
        clip.savedName = spec;
        clip.sequence = ResolveSequenceKey(sequenceNames, spec);
        if (clip.sequence < 0)
            std::cerr << "[viewer] no animation named '" << spec << "'\n";
    }
    return clip;
}

// A recipe file is the base and the flags override it — but only the flags the
// user typed, or a flag's default would overwrite what the recipe stored.
ExportRecipe BuildCaptureRecipe(ViewerApp& app, const CaptureCliOptions& c) {
    ExportRecipe recipe;
    const bool fromRecipe = !c.recipeFile.empty();
    const auto& names = app.Playback().SequenceNames();
    if (fromRecipe) {
        ExportRecipeLoadReport rep;
        if (!ReadExportRecipeFile(c.recipeFile, recipe, names, &rep))
            std::cerr << "Failed to read export recipe: " << io::PathToUtf8(c.recipeFile) << "\n";
        for (const std::string& missing : rep.unresolvedClips)
            std::cerr << "[viewer] recipe clip not in this model: " << missing << "\n";
    }

    if (!c.clips.empty()) {
        if (c.sequence != 0)
            std::printf("[viewer] --export-clip given; ignoring the positional index %d\n", c.sequence);
        recipe.clips.clear();
        for (const std::string& spec : c.clips)
            recipe.clips.push_back(ParseClipSpec(spec, names));
    } else if (recipe.clips.empty()) {
        ExportClip clip;
        clip.sequence = c.sequence;
        clip.savedName = SequenceKey(names, c.sequence);
        recipe.clips.push_back(std::move(clip));
    }

    if (c.fps || !fromRecipe)
        recipe.timing.fps = c.fps.value_or(30);
    if (c.durationMs > 0) {
        recipe.timing.duration = ExportDuration::Fixed;
        recipe.timing.durationMs = c.durationMs;
    }
    if (c.fill || !fromRecipe)
        recipe.timing.fill = c.fill.value_or(ExportFill::LoopLast);
    if (c.preRollMs > 0)
        recipe.timing.preRollMs = c.preRollMs;
    if (c.blendMs > 0)
        for (ExportClip& clip : recipe.clips)
            clip.blendMs = c.blendMs;
    if (c.frameStep > 1)
        recipe.timing.frameStep = c.frameStep;

    if (c.orbit) {
        recipe.camera.mode = ExportCameraMode::Orbit;
        recipe.camera.subject = c.subject;
        recipe.camera.angleCount = c.angles;
        recipe.camera.startYawDeg = c.orbitStart;
        recipe.camera.fitToBounds = c.orbitFit;
        if (c.orbitRevolutions) {
            recipe.camera.timing = OrbitTiming::Revolutions;
            recipe.camera.revolutions = *c.orbitRevolutions;
        } else {
            recipe.camera.timing = OrbitTiming::Velocity;
            recipe.camera.degPerSec = c.orbitDegPerSec;
        }
        if (c.orbitPitch) {
            recipe.camera.overridePitch = true;
            recipe.camera.pitchDeg = *c.orbitPitch;
        }
        if (c.orbitDistance > 0.0f) {
            recipe.camera.overrideDistance = true;
            recipe.camera.distance = c.orbitDistance;
        }
    } else if (c.camera >= 0) {
        recipe.camera.mode = ExportCameraMode::Preset;
        recipe.camera.preset = c.camera;
    }

    if (c.sheetColumns >= 0)
        recipe.output.format = ExportFormat::PngSheet;
    else if (c.format || !fromRecipe)
        recipe.output.format = c.format.value_or(ExportFormat::PngFrames);
    if (c.sheetColumns > 0)
        recipe.output.sheetColumns = c.sheetColumns;
    if (c.transparent || !fromRecipe)
        recipe.output.transparent = c.transparent.value_or(false);
    if (c.captureUi || !fromRecipe)
        recipe.output.captureUi = c.captureUi.value_or(false);
    if (c.resolution || !fromRecipe) {
        recipe.output.width = c.resolution ? c.resolution->first : 0;
        recipe.output.height = c.resolution ? c.resolution->second : 0;
    }
    if (c.crop)
        recipe.output.autoCrop = true;
    if (!fromRecipe) {
        // A bare headless run is a gate: overlays stay where the other flags put
        // them (--show-collisions must export its colliders) and no sidecar is
        // written beside a golden. A recipe says what it wants.
        recipe.output.hideOverlays = false;
        recipe.output.writeSidecar = false;
    }
    if (!c.nameTemplate.empty())
        recipe.output.nameTemplate = c.nameTemplate;
    if (!c.folder.empty())
        recipe.output.folder = c.folder;
    return recipe;
}

void CaptureAnimation(ViewerApp& app, renderer::SceneManager& scene, const CaptureCliOptions& c) {
    // Warm-up: drain the async asset pump and let a standalone `.pkb`'s particle
    // cloud develop, so the deferred effect framing reframes before the capture.
    for (i32 i = 0; i < tuning::kHeadlessWarmupTicks && !app.ShouldClose(); ++i) {
        scene.Update(tuning::kHeadlessTickSeconds);
        app.Tick(tuning::kHeadlessTickSeconds);
    }
    // Camera presets exist only once the model template has loaded.
    if (c.camera >= 0) {
        std::printf("[viewer] model has %zu camera preset(s); activating #%d\n",
                    app.Playback().CameraPresets().size(), c.camera);
        app.Playback().ActivateCameraPreset(c.camera);
    }
    app.Capture().Request(BuildCaptureRecipe(app, c));
    scene.Update(tuning::kHeadlessTickSeconds);
    app.Tick(tuning::kHeadlessTickSeconds); // runs the export synchronously
}

[[noreturn]] void CloseAndExit(ViewerApp& app, i32 code) {
    // Close() does the orderly GPU shutdown; static teardown after it crashes
    // (~RenderService destructs gfx services after the device is gone).
    app.Close();
    std::fflush(stdout);
    std::fflush(stderr);
    std::_Exit(code);
}

} // namespace

i32 RunViewer(renderer::RenderService& renderer, renderer::SceneManager& scene,
              const CliOptions& options, RunMode mode, gfx::GfxApi backend) {
    const bool interactive = mode == RunMode::Interactive;
    if (const char* flag = ModelFlagOf(mode); flag && options.model.empty()) {
        // A scripted run has nobody to answer a file picker.
        std::cerr << "[viewer] " << flag << " needs a model path\n";
        return 2;
    }

    ViewerApp app(renderer);
    const bool uiShot = mode == RunMode::UiShot;
    if (uiShot)
        app.SetDpiScaleOverride(1.0f);
    const i32 width = uiShot ? kUiShotWidth : ui::kDefaultWindowWidth;
    const i32 height = uiShot ? kUiShotHeight : ui::kDefaultWindowHeight;
    if (!app.Open(width, height, backend, /*visible=*/NeedsWindow(mode))) {
        std::cerr << "Failed to open viewer\n";
        return 1;
    }

    // The real sound emitter before LoadSettingsIni: that applies the persisted
    // volume, and the default NullSoundEmitter would swallow it.
    renderer.SwapSoundEmitter(std::make_unique<CubebSoundEmitter>(scene.ActiveContentProvider()));
    ApplySettings(app, renderer, options);

    // Also used by Settings > IO and File > Open, so initialised whatever the mode.
    NFD::Init();
    if (interactive && options.model.empty()) {
        if (auto picked = PickModelToOpen()) {
            CliOptions withPick = options;
            withPick.model = std::move(*picked);
            OpenDocuments(app, withPick, /*loadNow=*/!options.attachAnims.empty());
        }
    } else {
        // `--attach-anim` without scripted work still loads now: the attach
        // needs the model in hand.
        OpenDocuments(app, options, /*loadNow=*/!interactive || !options.attachAnims.empty());
    }

    switch (mode) {
    case RunMode::ListClips: {
        const i32 code = ListClips(app);
        app.Close();
        return code;
    }
    case RunMode::UiShot:
        CloseAndExit(app, RunUiShot(app, options.uiShot));
    case RunMode::ExportWem:
    case RunMode::ExportMdx:
    case RunMode::ExportM3:
    case RunMode::ExportGltf:
    case RunMode::SaveM3: {
        const bool ok = Convert(app, options.convert, mode);
        app.Close();
        return ok ? 0 : 1;
    }
    case RunMode::CaptureAnimation:
        CaptureAnimation(app, scene, options.capture);
        CloseAndExit(app, 0);
    default:
        break;
    }

    auto last = std::chrono::steady_clock::now();
    while (!app.ShouldClose()) {
        const auto now = std::chrono::steady_clock::now();
        const f32 dt = std::chrono::duration<f32>(now - last).count();
        last = now;
        scene.Update(dt);
        app.Tick(dt);
    }
    CloseAndExit(app, 0);
}

} // namespace whiteout::flakes::cli
