#include "cli/cli_options.h"

#include "backend_names.h"
#include "features/d3_visual_slots.h"
#include "io/wem/wem_profiles.h"
#include "whiteout/flakes/util/path_utf8.h"

#include <array>
#include <charconv>
#include <cstdlib>
#include <cstring>
#include <system_error>

namespace whiteout::flakes::cli {

namespace {

enum class Group : u8 { Viewer, Content, Conversion, Capture, Gates };

/// A row's values in order: `s` a string or path, `i` an integer, `f` a number.
struct Status {
    std::optional<i32> exitCode;
    std::string message;
    bool error = false;
};

using Values = std::span<const std::string>;
using Apply = Status (*)(CliOptions&, Values, std::vector<std::string>& warnings);

struct Option {
    std::string_view name;
    std::string_view alias;
    std::string_view values; ///< One kind letter per value; its length is the arity.
    Group group;
    std::string_view valueHelp;
    std::string_view help;
    Apply apply;
};

// Values reach the rows already checked against their kind (see Parse), so these
// only convert.
i32 Int(const std::string& v) {
    return std::atoi(v.c_str());
}
f32 Float(const std::string& v) {
    return static_cast<f32>(std::atof(v.c_str()));
}
std::filesystem::path Path(const std::string& v) {
    return io::FsPathFromUtf8(v);
}

// The whole string, with one optional sign: `-12` is a value, `12px` is not.
// from_chars reads a minus but not a plus, so the sign is skipped by hand.
bool Parses(std::string_view v, auto parsed) {
    if (!v.empty() && (v.front() == '-' || v.front() == '+'))
        v.remove_prefix(1);
    if (v.empty() || v.front() == '-' || v.front() == '+')
        return false;
    const auto [end, ec] = std::from_chars(v.data(), v.data() + v.size(), parsed);
    return ec == std::errc{} && end == v.data() + v.size();
}
bool IsInt(std::string_view v) {
    return Parses(v, i32{});
}
bool IsFloat(std::string_view v) {
    return Parses(v, f32{});
}

Status Fail(i32 code, std::string message) {
    return {code, std::move(message), true};
}

Status Usage(std::string_view flag, std::string_view problem) {
    return Fail(kUsageExitCode, "[viewer] " + std::string(flag) + ": " + std::string(problem) + "\n");
}

#define ROW_FLAG(field) [](CliOptions& o, Values, std::vector<std::string>&) { o.field = true; return Status{}; }
#define ROW_CLEAR(field) [](CliOptions& o, Values, std::vector<std::string>&) { o.field = false; return Status{}; }
#define ROW_STRING(field) [](CliOptions& o, Values v, std::vector<std::string>&) { o.field = v[0]; return Status{}; }
#define ROW_PATH(field) [](CliOptions& o, Values v, std::vector<std::string>&) { o.field = Path(v[0]); return Status{}; }
#define ROW_INT(field) [](CliOptions& o, Values v, std::vector<std::string>&) { o.field = Int(v[0]); return Status{}; }
#define ROW_FLOAT(field) [](CliOptions& o, Values v, std::vector<std::string>&) { o.field = Float(v[0]); return Status{}; }

// clang-format off
constexpr std::array kOptions = {
    // ---- Viewer ----
    Option{"--backend", "-b", "s", Group::Viewer, "<name>", "graphics backend for this run",
        [](CliOptions& o, Values v, std::vector<std::string>&) {
            if (const auto api = tools::BackendFromCliName(v[0])) {
                o.backend = *api;
                return Status{};
            }
            // On Linux, d3d11/d3d12 are not built: say so rather than silently
            // overriding, so the user knows the flag did nothing useful.
            return Fail(1, "Unknown / unsupported backend: " + v[0] +
                               " (valid on this platform: " + tools::BackendCliList() + ")\n");
        }},
    Option{"--wgpu-backend", "", "s", Group::Viewer, "<name>",
        "Dawn's adapter under --backend webgpu (d3d11|d3d12|vulkan|gl|metal)", ROW_STRING(wgpuBackend)},
    Option{"--wem-profile", "", "s", Group::Viewer, "<name>",
        "open a .wem as wc3_classic|wc3_reforged|wow|sc2|heroes|diablo3",
        [](CliOptions& o, Values v, std::vector<std::string>&) {
            o.wemProfile = io::WemProfileFromName(v[0]);
            if (o.wemProfile == wem::ProfileId::Count)
                return Fail(1, "Unknown WEM profile: " + v[0] +
                                   " (wc3_classic | wc3_reforged | wow | sc2 | heroes | diablo3)\n");
            return Status{};
        }},
    Option{"--attach-anim", "", "s", Group::Viewer, "<file.m3a>",
        "merge an animation file into the loaded .m3 (repeatable)",
        [](CliOptions& o, Values v, std::vector<std::string>&) {
            o.attachAnims.push_back(Path(v[0]));
            return Status{};
        }},
    Option{"--list-clips", "", "", Group::Viewer, "", "print the model's clips and where each came from, and exit",
        ROW_FLAG(listClips)},
    Option{"--show-collisions", "", "", Group::Viewer, "", "force the collision-shape overlay on",
        ROW_FLAG(showCollisions)},
    Option{"--ui-shot", "", "s", Group::Viewer, "<out.png>",
        "render the UI to a PNG in a hidden window and exit", ROW_PATH(uiShot.out)},
    Option{"--ui-shot-panel", "", "s", Group::Viewer, "<panel>",
        "what --ui-shot opens first (see harness/ui_shot.h)", ROW_STRING(uiShot.panel)},
    Option{"--help", "-h", "", Group::Viewer, "", "print this help and exit",
        [](CliOptions&, Values, std::vector<std::string>&) { return Status{0, HelpText(), false}; }},

    // ---- Content ----
    Option{"--listfile", "", "s", Group::Content, "<file.csv>",
        "World of Warcraft `id;path` listfile for headless runs", ROW_STRING(listfilePath)},
    Option{"--tact-keys", "", "s", Group::Content, "<file.txt>",
        "TACT key list for headless runs", ROW_STRING(tactKeyPath)},
    Option{"--content-root", "", "s", Group::Content, "<dir>",
        "loose asset tree searched before the archives", ROW_STRING(contentRoot)},

    // ---- Conversion ----
    Option{"--export-wem", "", "s", Group::Conversion, "<out.wem>", "write the model as WEM and exit",
        ROW_PATH(convert.wemPath)},
    Option{"--export-mdx", "", "s", Group::Conversion, "<out.mdx>",
        "write it as Warcraft III and exit", ROW_PATH(convert.mdxPath)},
    Option{"--export-mdx-profile", "", "s", Group::Conversion, "<name>",
        "wc3_reforged (default) | wc3_classic",
        [](CliOptions& o, Values v, std::vector<std::string>&) {
            o.convert.mdx.profile = io::WemProfileFromName(v[0]);
            if (o.convert.mdx.profile != wem::ProfileId::Wc3Classic &&
                o.convert.mdx.profile != wem::ProfileId::Wc3Reforged)
                return Fail(1, "Unknown Warcraft III profile: " + v[0] +
                                   " (wc3_classic | wc3_reforged)\n");
            return Status{};
        }},
    Option{"--export-mdx-no-textures", "", "", Group::Conversion, "",
        "do not write the textures beside it", ROW_CLEAR(convert.mdx.textures)},
    Option{"--export-m3", "", "s", Group::Conversion, "<out.m3>",
        "write it as StarCraft II and exit", ROW_PATH(convert.m3Path)},
    Option{"--export-m3-profile", "", "s", Group::Conversion, "<name>", "sc2 (default) | heroes",
        [](CliOptions& o, Values v, std::vector<std::string>&) {
            o.convert.m3.profile = io::WemProfileFromName(v[0]);
            if (o.convert.m3.profile != wem::ProfileId::Sc2 &&
                o.convert.m3.profile != wem::ProfileId::Heroes)
                return Fail(1, "Unknown StarCraft II profile: " + v[0] + " (sc2 | heroes)\n");
            return Status{};
        }},
    Option{"--export-m3-no-textures", "", "", Group::Conversion, "",
        "do not write the textures beside it", ROW_CLEAR(convert.m3.textures)},
    Option{"--export-m3-exact-passes", "", "", Group::Conversion, "",
        "Warcraft III: a draw per pass the fold would otherwise approximate",
        ROW_FLAG(convert.m3.exactPasses)},
    Option{"--export-m3-sharpen-key", "", "", Group::Conversion, "",
        "Warcraft III: bake keyed alpha binary", ROW_FLAG(convert.m3.sharpenTeamKey)},
    Option{"--export-m3-war3-mod-textures", "", "", Group::Conversion, "",
        "Warcraft III: name War3 (Mod)'s copy of a texture where it ships the same picture",
        ROW_FLAG(convert.m3.war3ModTextures)},
    Option{"--export-m3-no-effects", "", "", Group::Conversion, "",
        "Warcraft III: write no PAR_ / RIB_", ROW_CLEAR(convert.m3.effects)},
    Option{"--export-m3-no-standard-refs", "", "", Group::Conversion, "",
        "Warcraft III: add no Ref_Origin / Ref_Overhead / Ref_Center / Ref_Target / Vol_Target",
        ROW_CLEAR(convert.m3.standardRefs)},
    Option{"--export-gltf", "", "s", Group::Conversion, "<out.glb>",
        "write it as glTF 2.0 and exit (.gltf writes JSON + .bin + images)", ROW_PATH(convert.gltfPath)},
    Option{"--export-gltf-no-textures", "", "", Group::Conversion, "",
        "leave the texture URIs unresolved", ROW_CLEAR(convert.gltf.textures)},
    Option{"--save-m3", "", "s", Group::Conversion, "<out.m3>", "re-save an open .m3 and exit",
        ROW_PATH(convert.saveM3Path)},
    Option{"--save-m3-merge-anims", "", "", Group::Conversion, "", "fold the attached .m3a files in",
        ROW_FLAG(convert.saveM3.mergeAnimations)},
    Option{"--save-m3-sc2", "", "", Group::Conversion, "", "retarget a Heroes model for StarCraft II",
        ROW_FLAG(convert.saveM3.convertToSc2)},
    Option{"--save-m3-textures", "", "", Group::Conversion, "",
        "copy the referenced textures beside it", ROW_FLAG(convert.saveM3.textures)},

    // ---- Animation capture ----
    Option{"--export-anim", "", "iis", Group::Capture, "<seq> <fps> <folder>",
        "capture an animation headlessly and exit",
        [](CliOptions& o, Values v, std::vector<std::string>&) {
            o.capture.enabled = true;
            o.capture.sequence = Int(v[0]);
            o.capture.fps = Int(v[1]);
            o.capture.folder = Path(v[2]);
            return Status{};
        }},
    Option{"--export-recipe", "", "s", Group::Capture, "<recipe.ini>",
        "capture from a saved recipe; flags override it",
        [](CliOptions& o, Values v, std::vector<std::string>&) {
            o.capture.recipeFile = Path(v[0]);
            o.capture.enabled = true;
            return Status{};
        }},
    Option{"--export-clip", "", "s", Group::Capture, "<name|#index[:repeats][@speed]>",
        "queue a clip (repeatable); replaces the positional sequence",
        [](CliOptions& o, Values v, std::vector<std::string>&) {
            o.capture.clips.push_back(v[0]);
            return Status{};
        }},
    Option{"--export-duration", "", "i", Group::Capture, "<ms>", "fixed length; 0 is the queue's own",
        ROW_INT(capture.durationMs)},
    Option{"--export-fill", "", "s", Group::Capture, "<mode>",
        "loop-last (default) | loop-queue | hold-last",
        [](CliOptions& o, Values v, std::vector<std::string>&) {
            o.capture.fill = v[0] == "loop-queue"  ? ExportFill::LoopQueue
                             : v[0] == "hold-last" ? ExportFill::HoldLast
                                                   : ExportFill::LoopLast;
            return Status{};
        }},
    Option{"--export-preroll", "", "i", Group::Capture, "<ms>", "simulate before frame 0",
        ROW_INT(capture.preRollMs)},
    Option{"--export-blend", "", "i", Group::Capture, "<ms>", "cross-fade into every clip",
        ROW_INT(capture.blendMs)},
    Option{"--export-step", "", "i", Group::Capture, "<n>", "capture every Nth frame",
        ROW_INT(capture.frameStep)},
    Option{"--export-orbit", "", "f", Group::Capture, "<deg/s>", "orbit at this velocity",
        [](CliOptions& o, Values v, std::vector<std::string>&) {
            o.capture.orbit = true;
            o.capture.orbitDegPerSec = Float(v[0]);
            return Status{};
        }},
    Option{"--export-orbit-revs", "", "f", Group::Capture, "<n>", "orbit this many times over the capture",
        [](CliOptions& o, Values v, std::vector<std::string>&) {
            o.capture.orbit = true;
            o.capture.orbitRevolutions = Float(v[0]);
            return Status{};
        }},
    Option{"--export-orbit-pitch", "", "f", Group::Capture, "<deg>", "orbit pitch",
        [](CliOptions& o, Values v, std::vector<std::string>&) {
            o.capture.orbitPitch = Float(v[0]);
            return Status{};
        }},
    Option{"--export-orbit-dist", "", "f", Group::Capture, "<units>", "orbit distance",
        ROW_FLOAT(capture.orbitDistance)},
    Option{"--export-orbit-start", "", "f", Group::Capture, "<deg>", "orbit start yaw",
        ROW_FLOAT(capture.orbitStart)},
    Option{"--export-fit", "", "", Group::Capture, "", "fit the orbit to the model's bounds",
        ROW_FLAG(capture.orbitFit)},
    Option{"--export-subject", "", "s", Group::Capture, "<what>", "camera (default) | model",
        [](CliOptions& o, Values v, std::vector<std::string>&) {
            o.capture.subject = v[0] == "model" ? OrbitSubject::Model : OrbitSubject::Camera;
            return Status{};
        }},
    Option{"--export-angles", "", "i", Group::Capture, "<n>", "render N orbit angles",
        [](CliOptions& o, Values v, std::vector<std::string>&) {
            o.capture.orbit = true;
            o.capture.angles = Int(v[0]);
            return Status{};
        }},
    Option{"--export-sheet", "", "s", Group::Capture, "<cols|auto>", "write a sprite sheet",
        [](CliOptions& o, Values v, std::vector<std::string>&) {
            if (v[0] != "auto" && !IsInt(v[0]))
                return Usage("--export-sheet", "expected a column count or auto, got '" + v[0] + "'");
            o.capture.sheetColumns = v[0] == "auto" ? 0 : Int(v[0]);
            return Status{};
        }},
    Option{"--export-crop", "", "", Group::Capture, "", "crop every frame to the model",
        ROW_FLAG(capture.crop)},
    Option{"--export-name", "", "s", Group::Capture, "<template>", "output name template",
        ROW_STRING(capture.nameTemplate)},
    Option{"--gif", "", "", Group::Capture, "", "animated GIF",
        [](CliOptions& o, Values, std::vector<std::string>&) { o.capture.format = ExportFormat::Gif; return Status{}; }},
    Option{"--apng", "", "", Group::Capture, "", "animated PNG",
        [](CliOptions& o, Values, std::vector<std::string>&) { o.capture.format = ExportFormat::Apng; return Status{}; }},
    Option{"--webp", "", "", Group::Capture, "", "animated WebP",
        [](CliOptions& o, Values, std::vector<std::string>&) { o.capture.format = ExportFormat::Webp; return Status{}; }},
    Option{"--mp4", "", "", Group::Capture, "", "H.264 through ffmpeg",
        [](CliOptions& o, Values, std::vector<std::string>&) { o.capture.format = ExportFormat::Mp4; return Status{}; }},
    Option{"--webm", "", "", Group::Capture, "", "VP9 through ffmpeg",
        [](CliOptions& o, Values, std::vector<std::string>&) { o.capture.format = ExportFormat::WebmVp9; return Status{}; }},
    Option{"--transparent", "", "", Group::Capture, "", "key the background out",
        [](CliOptions& o, Values, std::vector<std::string>&) { o.capture.transparent = true; return Status{}; }},
    Option{"--ui", "", "", Group::Capture, "", "composite the viewer UI into each frame",
        [](CliOptions& o, Values, std::vector<std::string>&) { o.capture.captureUi = true; return Status{}; }},
    Option{"--res", "", "ii", Group::Capture, "<w> <h>", "output resolution",
        [](CliOptions& o, Values v, std::vector<std::string>&) {
            o.capture.resolution = std::pair{Int(v[0]), Int(v[1])};
            return Status{};
        }},
    Option{"--camera", "", "i", Group::Capture, "<index>", "capture through a model camera preset",
        ROW_INT(capture.camera)},

    // ---- Gates ----
    Option{"--headless-test", "", "", Group::Gates, "", "offscreen render smoke test", ROW_FLAG(headlessTest)},
    Option{"--multiscene-test", "", "", Group::Gates, "", "two scenes, no cross-bleed", ROW_FLAG(multiSceneTest)},
    Option{"--childmodel-check", "", "", Group::Gates, "", "PE1 Birth/Death balance", ROW_FLAG(childModelCheck)},
    Option{"--particle-diff", "", "", Group::Gates, "", "particle trace record/check", ROW_FLAG(particleDiff)},
    Option{"--trace-record", "", "s", Group::Gates, "<file>", "--particle-diff: write a baseline",
        ROW_STRING(particleDiffOptions.recordPath)},
    Option{"--trace-check", "", "s", Group::Gates, "<file>", "--particle-diff: compare to a baseline",
        ROW_STRING(particleDiffOptions.checkPath)},
    Option{"--trace-frames", "", "i", Group::Gates, "<n>", "frames the gate harnesses capture",
        ROW_INT(traceFrames)},
    Option{"--trace-curve-tol", "", "", Group::Gates, "", "--particle-diff: curve tolerance",
        ROW_FLAG(particleDiffOptions.curveTolerance)},
    Option{"--trace-dump", "", "", Group::Gates, "", "--particle-diff: print the last frame",
        ROW_FLAG(particleDiffOptions.dump)},
    Option{"--trace-no-device", "", "", Group::Gates, "", "--particle-diff: tick with no device",
        ROW_CLEAR(particleDiffOptions.useDevice)},
    Option{"--draw-trace", "", "", Group::Gates, "", "draw trace record/check (G1/G2)", ROW_FLAG(drawTrace)},
    Option{"--draw-trace-record", "", "s", Group::Gates, "<file>", "write a trace baseline",
        ROW_STRING(drawTraceOptions.recordPath)},
    Option{"--draw-trace-check", "", "s", Group::Gates, "<file>", "compare to a trace baseline",
        ROW_STRING(drawTraceOptions.checkPath)},
    Option{"--draw-trace-golden", "", "s", Group::Gates, "<file.raw>", "record / compare the readback",
        ROW_STRING(drawTraceOptions.goldenPath)},
    Option{"--draw-trace-hd", "", "", Group::Gates, "", "render HD", ROW_FLAG(drawTraceOptions.hd)},
    Option{"--draw-trace-sd-hdr", "", "", Group::Gates, "", "SD through the HDR target + tonemap",
        ROW_FLAG(drawTraceOptions.sdHdr)},
    Option{"--draw-trace-debug-view", "", "i", Group::Gates, "<view>", "a DebugView value",
        ROW_INT(drawTraceOptions.debugView)},
    Option{"--draw-trace-select", "", "i", Group::Gates, "<stride>", "select every Nth vertex/face",
        ROW_INT(drawTraceOptions.selectStride)},
    Option{"--draw-trace-unlit", "", "", Group::Gates, "", "draw odd geosets unlit",
        ROW_FLAG(drawTraceOptions.unlitOddGeosets)},
    Option{"--no-cloth-deform", "", "", Group::Gates, "", "leave cloth geosets on their skinning",
        ROW_FLAG(drawTraceOptions.noClothDeform)},
    Option{"--draw-trace-no-refraction", "", "", Group::Gates, "", "WoW refraction pass off",
        ROW_FLAG(drawTraceOptions.noRefraction)},
    Option{"--draw-trace-refraction-mask", "", "", Group::Gates, "", "show the refraction mask",
        ROW_FLAG(drawTraceOptions.refractionMask)},
    Option{"--draw-trace-no-distortion", "", "", Group::Gates, "", "Diablo III distortion off",
        ROW_FLAG(drawTraceOptions.noDistortion)},
    Option{"--draw-trace-distortion-buffer", "", "", Group::Gates, "", "show the distortion buffer",
        ROW_FLAG(drawTraceOptions.distortionBuffer)},
    Option{"--draw-trace-no-multitex", "", "", Group::Gates, "", "WoW multi-texture particles off",
        ROW_FLAG(drawTraceOptions.noMultiTex)},
    Option{"--draw-trace-debug-light", "", "", Group::Gates, "", "one scripted point light",
        ROW_FLAG(drawTraceOptions.debugLight)},
    Option{"--draw-trace-shadows", "", "", Group::Gates, "", "cascade shadows on",
        ROW_FLAG(drawTraceOptions.shadows)},
    Option{"--draw-trace-fog", "", "i", Group::Gates, "<mode>", "one fixed world fog per shader mode",
        ROW_INT(drawTraceOptions.fogMode)},
    Option{"--draw-trace-allow-late-assets", "", "", Group::Gates, "", "do not abort on mid-capture assets",
        ROW_FLAG(drawTraceOptions.allowLateAssets)},
    Option{"--draw-trace-lazy-anim", "", "", Group::Gates, "", "stream `.anim` files",
        ROW_FLAG(drawTraceOptions.lazyAnim)},
    Option{"--draw-trace-game", "", "s", Group::Gates, "<game>", "wc3 | wow | sc2 | d3: resolve SNO/ids through that install",
        ROW_STRING(drawTraceOptions.game)},
    Option{"--draw-trace-distance-tol", "", "f", Group::Gates, "<units>", "compare distances with a tolerance",
        ROW_FLOAT(drawTraceOptions.distanceTol)},
    Option{"--draw-trace-camera-distance", "", "i", Group::Gates, "<units>", "camera distance (350)",
        ROW_INT(drawTraceOptions.cameraDistance)},
    Option{"--draw-trace-camera-yaw", "", "f", Group::Gates, "<rad>", "camera yaw (0.7)",
        ROW_FLOAT(drawTraceOptions.cameraYaw)},
    Option{"--draw-trace-perturb", "", "i", Group::Gates, "<seed>", "reseed actor ids",
        ROW_INT(drawTraceOptions.perturbSeed)},
    Option{"--draw-trace-instances", "", "i", Group::Gates, "<n>", "actors spawned (3)",
        ROW_INT(drawTraceOptions.instances)},
    Option{"--draw-trace-anim", "", "s", Group::Gates, "<seq>", "start sequence (index or name)",
        ROW_STRING(drawTraceOptions.anim.sequence)},
    Option{"--draw-trace-anim-switch", "", "is", Group::Gates, "<frame> <seq>", "switch sequence at a frame",
        [](CliOptions& o, Values v, std::vector<std::string>&) {
            o.drawTraceOptions.anim.switchFrame = Int(v[0]);
            o.drawTraceOptions.anim.switchSequence = v[1];
            return Status{};
        }},
    Option{"--draw-trace-anim-layer", "", "is", Group::Gates, "<frame> <seq>", "layer a sequence at a frame",
        [](CliOptions& o, Values v, std::vector<std::string>&) {
            o.drawTraceOptions.anim.layerFrame = Int(v[0]);
            o.drawTraceOptions.anim.layerSequence = v[1];
            return Status{};
        }},
    Option{"--draw-trace-anim-blend", "", "i", Group::Gates, "<ms>", "layer blend-in",
        ROW_INT(drawTraceOptions.anim.layerBlendInMs)},
    Option{"--draw-trace-anim-weight", "", "f", Group::Gates, "<w>", "layer weight",
        ROW_FLOAT(drawTraceOptions.anim.layerWeight)},
    Option{"--draw-trace-anim-subtrack", "", "i", Group::Gates, "<index>", "layer one sub-track",
        ROW_INT(drawTraceOptions.anim.layerSubtrack)},
    Option{"--draw-trace-anim-no-globals", "", "", Group::Gates, "", "silence global loops",
        ROW_FLAG(drawTraceOptions.anim.noGlobals)},
    Option{"--draw-trace-anim-list", "", "", Group::Gates, "", "print the sequence table and stop",
        ROW_FLAG(drawTraceOptions.anim.list)},
    Option{"--draw-trace-particle-list", "", "", Group::Gates, "", "print every PAR_ and stop",
        ROW_FLAG(drawTraceOptions.anim.particleList)},
    Option{"--draw-trace-attach-list", "", "", Group::Gates, "", "print every ATT_ after frame 0",
        ROW_FLAG(drawTraceOptions.anim.attachList)},
    Option{"--draw-trace-ribbon-list", "", "", Group::Gates, "", "print every RIB_ and stop",
        ROW_FLAG(drawTraceOptions.anim.ribbonList)},
    Option{"--draw-trace-anim-probe", "", "", Group::Gates, "", "print skinning plumbing and pose hashes",
        ROW_FLAG(drawTraceOptions.anim.probe)},
    Option{"--draw-trace-solvers", "", "", Group::Gates, "", "pose solvers on, with a ground plane",
        ROW_FLAG(drawTraceOptions.anim.solvers)},
    Option{"--draw-trace-ragdoll", "", "", Group::Gates, "", "collapse a Diablo III rig after the settle",
        ROW_FLAG(drawTraceOptions.anim.ragdoll)},
    Option{"--draw-trace-ground", "", "f", Group::Gates, "<z>", "solver ground height",
        ROW_FLOAT(drawTraceOptions.anim.groundZ)},
    Option{"--draw-trace-aim", "", "fff", Group::Gates, "<x> <y> <z>", "solver aim target, model space",
        [](CliOptions& o, Values v, std::vector<std::string>&) {
            auto& anim = o.drawTraceOptions.anim;
            anim.hasAim = true;
            anim.aim.x = Float(v[0]);
            anim.aim.y = Float(v[1]);
            anim.aim.z = Float(v[2]);
            return Status{};
        }},
    Option{"--d3-equip", "", "s", Group::Gates, "<slot>=<item>", "dress a Diablo III character (repeatable)",
        [](CliOptions& o, Values v, std::vector<std::string>& warnings) {
            const auto eq = v[0].find('=');
            if (eq == std::string::npos)
                return Status{};
            const std::string slotName = v[0].substr(0, eq);
            if (const auto slot = D3VisualSlotFromCliName(slotName))
                o.drawTraceOptions.anim.d3Equip.emplace_back(*slot, v[0].substr(eq + 1));
            else
                warnings.push_back("[dtrace] --d3-equip: bad slot '" + slotName + "'\n");
            return Status{};
        }},
    Option{"--d3-dye", "", "s", Group::Gates, "<slot>=<row>", "dye an equipped slot (repeatable)",
        [](CliOptions& o, Values v, std::vector<std::string>&) {
            const auto eq = v[0].find('=');
            if (eq == std::string::npos)
                return Status{};
            const std::string row = v[0].substr(eq + 1);
            if (!IsInt(row))
                return Usage("--d3-dye", "expected <slot>=<row> with a numeric row, got '" + v[0] + "'");
            if (const auto slot = D3VisualSlotFromCliName(v[0].substr(0, eq)))
                o.drawTraceOptions.anim.d3Dyes.emplace_back(*slot, Int(row));
            return Status{};
        }},
    Option{"--d3-sheathed", "", "", Group::Gates, "", "sheathe the equipped weapons",
        ROW_FLAG(drawTraceOptions.anim.d3Sheathed)},
};
// clang-format on

#undef ROW_FLAG
#undef ROW_CLEAR
#undef ROW_STRING
#undef ROW_PATH
#undef ROW_INT
#undef ROW_FLOAT

const Option* FindOption(std::string_view arg) {
    for (const Option& o : kOptions)
        if (arg == o.name || (!o.alias.empty() && arg == o.alias))
            return &o;
    return nullptr;
}

constexpr std::string_view GroupTitle(Group g) {
    switch (g) {
    case Group::Viewer:
        return "Viewer";
    case Group::Content:
        return "Content (headless runs)";
    case Group::Conversion:
        return "Conversion";
    case Group::Capture:
        return "Animation capture";
    case Group::Gates:
        return "Gates";
    }
    return {};
}

} // namespace

ParseResult Parse(std::span<const std::string> args) {
    ParseResult result;
    CliOptions& o = result.options;
    std::vector<std::string> warnings;
    const auto flushWarnings = [&] {
        for (const std::string& w : warnings)
            result.warnings += w;
        warnings.clear();
    };

    const auto stop = [&](const Status& status) {
        flushWarnings();
        result.exitCode = status.exitCode;
        result.message = status.message;
        result.messageIsError = status.error;
        return result;
    };

    for (usize i = 0; i < args.size(); ++i) {
        const std::string& arg = args[i];
        if (const Option* option = FindOption(arg)) {
            const usize arity = option->values.size();
            if (i + arity >= args.size())
                return stop(Usage(arg, "expected " + std::string(option->valueHelp)));
            const Values values = args.subspan(i + 1, arity);
            for (usize k = 0; k < arity; ++k) {
                const std::string& value = values[k];
                // Negative numbers stay values; a double dash is the next flag,
                // which a short argument list would otherwise swallow.
                if (value.rfind("--", 0) == 0)
                    return stop(Usage(arg, "expected " + std::string(option->valueHelp) +
                                                        ", got the flag '" + value + "'"));
                const char kind = option->values[k];
                if ((kind == 'i' && !IsInt(value)) || (kind == 'f' && !IsFloat(value)))
                    return stop(Usage(arg, "'" + value + "' is not a number"));
            }
            const Status status = option->apply(o, values, warnings);
            i += arity;
            if (status.exitCode)
                return stop(status);
            continue;
        }
        // Anything that is not an option is a model path.
        if (o.model.empty())
            o.model = io::FsPathFromUtf8(arg);
        else
            o.extraModels.push_back(io::FsPathFromUtf8(arg));
    }
    flushWarnings();
    return result;
}

std::string HelpText() {
    std::string out = "Usage: WhiteoutFlakes [options] [<model-path>...]\n";
    for (const Group group :
         {Group::Viewer, Group::Content, Group::Conversion, Group::Capture, Group::Gates}) {
        out += "\n";
        out += GroupTitle(group);
        out += ":\n";
        for (const Option& o : kOptions) {
            if (o.group != group)
                continue;
            std::string left = "  ";
            left += o.name;
            if (!o.alias.empty()) {
                left += ", ";
                left += o.alias;
            }
            if (!o.valueHelp.empty()) {
                left += ' ';
                left += o.valueHelp;
            }
            constexpr usize kHelpColumn = 36;
            out += left;
            out += left.size() < kHelpColumn ? std::string(kHelpColumn - left.size(), ' ') : "  ";
            out += o.help;
            out += '\n';
        }
    }
    return out;
}

std::optional<ProductId> TraceGameFromName(std::string_view name) {
    if (name == "wc3")
        return ProductId::Wc3;
    if (name == "wow")
        return ProductId::Wow;
    if (name == "sc2")
        return ProductId::Sc2;
    if (name == "d3")
        return ProductId::D3;
    return std::nullopt;
}

RunMode SelectRunMode(const CliOptions& o) {
    if (o.headlessTest)
        return RunMode::HeadlessTest;
    if (o.multiSceneTest)
        return RunMode::MultiSceneTest;
    if (o.childModelCheck)
        return RunMode::ChildModelCheck;
    if (o.particleDiff)
        return RunMode::ParticleDiff;
    if (o.drawTrace)
        return RunMode::DrawTrace;
    if (o.listClips)
        return RunMode::ListClips;
    if (!o.uiShot.out.empty())
        return RunMode::UiShot;
    if (!o.convert.wemPath.empty())
        return RunMode::ExportWem;
    if (!o.convert.mdxPath.empty())
        return RunMode::ExportMdx;
    if (!o.convert.m3Path.empty())
        return RunMode::ExportM3;
    if (!o.convert.gltfPath.empty())
        return RunMode::ExportGltf;
    if (!o.convert.saveM3Path.empty())
        return RunMode::SaveM3;
    if (o.capture.enabled)
        return RunMode::CaptureAnimation;
    return RunMode::Interactive;
}

} // namespace whiteout::flakes::cli
