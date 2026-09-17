#pragma once

// ============================================================================
// The viewer's command line: every option as one table row, parsed into
// CliOptions, and the run mode the options select.
//
// Scripts drive the exe by these flags (render-diff.ps1, war3-diff.ps1,
// particle-diff.ps1, sc2-particle-coverage.ps1, the viewer gates), so a row's
// name, alias, arity and effect are a contract. Device-free:
// viewer_cli_options_test replays tests/data/viewer_cli_characterisation.txt.
// ============================================================================

#include "export/export_options.h"
#include "capture/export_recipe.h"
#include "harness/ui_shot.h"
#include "whiteout/flakes/enums.h"
#include "whiteout/flakes/gfx_types.h"
#include "whiteout/flakes/types.h"

#include <whiteout/models/wem/profile.h>

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace whiteout::flakes::cli {

namespace wem = ::whiteout::models::wem;

// ---- Gates -------------------------------------------------------------------

/// Gate G5's scripted scenario. The `.mdx` and `.m2` arms need nothing like it —
/// they play whatever sequence the model opens on and cut hard between them, so
/// "spawn and let it run" already covers their whole playback surface.
/// StarCraft II's does not: layered plays, blend envelopes and cross-fades only
/// exist once something asks for a second sequence.
///
/// Everything is frame-indexed rather than time-indexed on purpose. The
/// capture's dt is fixed, so a frame number is an exact millisecond, and a
/// baseline recorded today stays reproducible if the dt ever changes shape.
struct AnimScenario {
    /// Index or (case-insensitive substring of a) name. Empty leaves whatever
    /// the model opens on, which is what the geometry arm records.
    std::string sequence;
    /// Hard-ish switch: `SetActiveSequence`, so it takes the format's own
    /// transition policy — a cut for WC3/WoW, a cross-fade for M3.
    i32 switchFrame = -1;
    std::string switchSequence;
    /// Additive layer: a second play stacked on the first, the only way to reach
    /// the weight-budget blender.
    i32 layerFrame = -1;
    std::string layerSequence;
    i32 layerBlendInMs = 250;
    f32 layerWeight = 1.0f;
    /// One sub-track container of the layered sequence, or -1 for all of them.
    /// The Marine's shield is `Cover` sub-track 0; playing the whole of `Cover`
    /// also runs `Cover_full`, so only this can isolate a prop.
    i32 layerSubtrack = -1;
    /// Silence the model's global loops. The engine plays them, and silencing
    /// is how a baseline pins one sequence on its own.
    bool noGlobals = false;
    /// Print the sequence table and stop: sequence indices are export order and
    /// differ per model, so the corpus names sequences and this lists them.
    bool list = false;
    /// Print every `PAR_`'s statics and stop — the SC2 particle content survey
    /// (SC2_PARTICLE_PLAN.md §5).
    bool particleList = false;
    /// Print every `ATT_` after the first frame: name, bone, the bone's
    /// model-space position and rest visibility (WC3_TO_SC2_COMPLETION_PLAN.md
    /// C3.5).
    bool attachList = false;
    /// Print every `RIB_` as registration sees it and stop (C7.5).
    bool ribbonList = false;
    /// Turn the pose stages on and install a ground plane, so a capture can see
    /// terrain IK and the turret. Off in every other arm, which is what makes
    /// the byte-identical baselines mean "the animation did not move".
    bool solvers = false;
    /// Ground plane height for the solver arm. Non-zero is the interesting case:
    /// a plane at the model's own feet is what the tolerance skip declines.
    f32 groundZ = 0.0f;
    /// Aim target for turrets, in model space. Only used with @ref solvers.
    bool hasAim = false;
    Vector3f aim{0.0f, 0.0f, 0.0f};
    /// Collapse a Diablo III rigid rig after the settle. D3 builds one on a
    /// gameplay event, not at load, so nothing else can tell a rig that failed
    /// to build from one nobody armed.
    bool ragdoll = false;
    /// Dress a Diablo III player character by item name before the settle:
    /// (EVisualSlot ordinal, GameBalance item name).
    std::vector<std::pair<i32, std::string>> d3Equip;
    /// (EVisualSlot ordinal, dye row), applied after the equips.
    std::vector<std::pair<i32, i32>> d3Dyes;
    bool d3Sheathed = false;
    /// Print the skinning plumbing and a per-frame pose hash. A frozen palette
    /// renders a healthy bind pose; a hash that never moves is the cheap tell.
    bool probe = false;

    bool Any() const {
        return !sequence.empty() || switchFrame >= 0 || layerFrame >= 0 || noGlobals || list ||
               probe || solvers || ragdoll || !d3Equip.empty();
    }
};

/// `--draw-trace <model>`: gates G1 (draw trace) and G2 (golden image).
struct DrawTraceOptions {
    std::string recordPath;
    std::string checkPath;
    std::string goldenPath;
    bool hd = false;
    /// Route SD through the HDR scene target + tonemap, as the interactive viewer
    /// does for D3. Off by default so the recorded baselines keep their meaning.
    bool sdHdr = false;
    /// A DebugView value (include/whiteout/flakes/enums.h); -1 leaves it off.
    i32 debugView = -1;
    /// Select every Nth vertex and triangle of every geoset (the mesh overlay's
    /// selection arm); 0 selects nothing.
    i32 selectStride = 0;
    bool unlitOddGeosets = false;
    bool noClothDeform = false;
    bool noRefraction = false;
    bool refractionMask = false;
    bool noDistortion = false;
    bool distortionBuffer = false;
    bool noMultiTex = false;
    bool debugLight = false;
    bool shadows = false;
    /// A shader fog mode (bls::FogParams::mode); 0 is off.
    i32 fogMode = 0;
    bool lazyAnim = false;
    bool allowLateAssets = false;
    f32 distanceTol = 0.0f;
    i32 cameraDistance = 350;
    f32 cameraYaw = 0.7f;
    i32 perturbSeed = 0;
    i32 instances = 3;
    /// `wc3 | wow | sc2 | d3`, as typed; validated when the run starts. Empty
    /// is the corpus arm: nothing resolves beyond the file it was handed.
    std::string game;
    AnimScenario anim;
};

/// `--particle-diff <model>`: the L1/L2 particle trace harness.
struct ParticleDiffOptions {
    std::string recordPath;
    std::string checkPath;
    bool curveTolerance = false;
    bool dump = false;
    bool useDevice = true;
};

// ---- Conversion -------------------------------------------------------------

struct ConvertOptions {
    std::filesystem::path wemPath;

    std::filesystem::path mdxPath;
    MdxExportOptions mdx;

    std::filesystem::path m3Path;
    M3ExportOptions m3;

    /// `.gltf` writes JSON + .bin + images; anything else one `.glb`
    /// (`gltf.binary` is decided from this path when the export runs).
    std::filesystem::path gltfPath;
    GltfExportOptions gltf;

    /// Save As for a model that IS `.m3`.
    std::filesystem::path saveM3Path;
    M3SaveOptions saveM3;
};

// ---- Animation capture ----------------------------------------------------------

/// `--export-anim` and its modifiers. A recipe file is the base and flags
/// override it, so the overrides are optional: only what the user typed may
/// overwrite what the recipe stored.
struct CaptureCliOptions {
    bool enabled = false;
    /// The positional sequence index; ignored when any `--export-clip` is given.
    i32 sequence = 0;
    std::optional<i32> fps;
    std::filesystem::path folder;
    /// `name|#index[:repeats][@speed]`.
    std::vector<std::string> clips;
    i32 durationMs = 0; ///< 0 = the queue's own length
    std::optional<ExportFill> fill;
    i32 preRollMs = 0;
    /// Applied to every clip start, so a queue can cross-fade without a recipe.
    i32 blendMs = 0;
    i32 frameStep = 1;
    bool orbit = false;
    f32 orbitDegPerSec = 0.0f;
    std::optional<f32> orbitRevolutions;
    std::optional<f32> orbitPitch;
    f32 orbitDistance = 0.0f;
    f32 orbitStart = 0.0f;
    bool orbitFit = false;
    OrbitSubject subject = OrbitSubject::Camera;
    i32 angles = 1;
    i32 sheetColumns = -1; ///< -1 = not a sheet, 0 = auto
    bool crop = false;
    std::string nameTemplate;
    std::filesystem::path recipeFile;
    std::optional<ExportFormat> format;
    std::optional<bool> transparent;
    std::optional<bool> captureUi;
    std::optional<std::pair<i32, i32>> resolution;
    i32 camera = -1; ///< -1 = free camera; >= 0 = model camera preset index
};

// ---- Everything ----------------------------------------------------------------

struct CliOptions {
    std::optional<gfx::GfxApi> backend;
    /// Dawn's adapter backend (d3d11/d3d12/vulkan/gl/metal), for `--backend webgpu`.
    std::string wgpuBackend;

    /// The first positional path, and the rest, each opened in its own tab.
    std::filesystem::path model;
    std::vector<std::filesystem::path> extraModels;

    /// World of Warcraft's `id;path` CSV, the TACT key list and the loose tree
    /// searched before the archives. The GUI takes these from Settings > IO;
    /// headless runs happen before those apply.
    std::string listfilePath;
    std::string tactKeyPath;
    std::string contentRoot;

    /// Which profile a `.wem` opens as. `Count` leaves it to the document.
    wem::ProfileId wemProfile = wem::ProfileId::Count;
    /// `.m3a` files merged into the loaded `.m3` before anything else runs.
    std::vector<std::filesystem::path> attachAnims;
    bool listClips = false;
    /// Force the collision-shape overlay on, so a headless capture shows it.
    bool showCollisions = false;

    bool headlessTest = false;
    bool multiSceneTest = false;
    bool childModelCheck = false;
    bool particleDiff = false;
    bool drawTrace = false;
    /// `--trace-frames`: shared by the particle diff, the draw trace and the
    /// child-model check.
    i32 traceFrames = 120;
    DrawTraceOptions drawTraceOptions;
    ParticleDiffOptions particleDiffOptions;

    ConvertOptions convert;
    CaptureCliOptions capture;
    UiShotOptions uiShot;
};

/// What one run of the exe does. Picked from the options in a fixed precedence;
/// every scripted mode states whether it needs a visible window.
enum class RunMode : u8 {
    Interactive,
    HeadlessTest,
    MultiSceneTest,
    ChildModelCheck,
    ParticleDiff,
    DrawTrace,
    ListClips,
    UiShot,
    ExportWem,
    ExportMdx,
    ExportM3,
    ExportGltf,
    SaveM3,
    CaptureAnimation,
};

RunMode SelectRunMode(const CliOptions& options);

/// Only the interactive viewer is shown. A scripted mode that needs the full
/// app runs it in a hidden window, so a corpus sweep never flashes or steals
/// focus.
constexpr bool NeedsWindow(RunMode mode) {
    return mode == RunMode::Interactive;
}

/// The modes that dispatch before the viewer app exists and never open a window.
constexpr bool IsGateHarness(RunMode mode) {
    return mode == RunMode::HeadlessTest || mode == RunMode::MultiSceneTest ||
           mode == RunMode::ChildModelCheck || mode == RunMode::ParticleDiff ||
           mode == RunMode::DrawTrace;
}

/// A command line the parser rejects: a missing value, a value that is another
/// flag, a non-number where a number goes.
inline constexpr i32 kUsageExitCode = 2;

struct ParseResult {
    CliOptions options;
    /// Printed to stderr whatever happens next, e.g. an unknown `--d3-equip` slot.
    std::string warnings;
    /// Set when the command line already decided the run: `--help` (0) or a
    /// rejected value.
    std::optional<i32> exitCode;
    /// What to print before exiting: the help text (stdout) or the reason (stderr).
    std::string message;
    bool messageIsError = false;
};

/// @p args excludes the program name.
ParseResult Parse(std::span<const std::string> args);

/// The `--help` text, grouped, one line per option.
std::string HelpText();

/// `vk` / `D3D12` / ... through the backend table, `wc3 | wow | sc2 | d3` for
/// `--draw-trace-game`.
std::optional<ProductId> TraceGameFromName(std::string_view name);

} // namespace whiteout::flakes::cli
