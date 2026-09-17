// ============================================================================
// The viewer's command-line parser (tools/basic_viewer/cli/cli_options.h),
// replayed against what the pre-table parser did with the same argv.
//
// tests/data/viewer_cli_characterisation.txt was recorded by
// scripts/viewer-cli-characterise.ps1 from the `strcmp` chain this table
// replaced (BASIC_VIEWER_REFACTOR_PLAN.md P0.5): one block per argument vector,
// holding the exit code and either the lines of its `--dump-cli` state that
// differ from the no-argument run, or whatever else it printed. The dump below
// spells the same keys from CliOptions, so the two can be compared line by line.
// ============================================================================

#include "cli/cli_options.h"

#include "whiteout/flakes/util/path_utf8.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using namespace whiteout::flakes;
using namespace whiteout::flakes::cli;

namespace {

struct Case {
    std::string name;
    std::vector<std::string> argv;
    int exit = 0;
    bool dump = false; ///< `dump:` (a diff against the no-args dump) or `out:`
    std::vector<std::string> lines;
};

std::vector<Case> LoadCases() {
    std::ifstream in(WDX_VIEWER_CLI_CHARACTERISATION, std::ios::binary);
    REQUIRE(in);
    std::vector<Case> cases;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.rfind("=== ", 0) == 0) {
            cases.emplace_back().name = line.substr(4);
        } else if (line.rfind("argv: ", 0) == 0) {
            std::string rest = line.substr(6);
            usize start = 0;
            while (!rest.empty() && start <= rest.size()) {
                const usize tab = rest.find('\t', start);
                cases.back().argv.push_back(rest.substr(start, tab - start));
                if (tab == std::string::npos)
                    break;
                start = tab + 1;
            }
        } else if (line.rfind("exit: ", 0) == 0) {
            cases.back().exit = std::stoi(line.substr(6));
        } else if (line == "dump:" || line == "out:") {
            cases.back().dump = line == "dump:";
        } else if (line.rfind("  ", 0) == 0 && !cases.empty()) {
            cases.back().lines.push_back(line.substr(2));
        }
    }
    return cases;
}

std::vector<std::string> SplitLines(const std::string& text) {
    std::vector<std::string> out;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line))
        if (!line.empty())
            out.push_back(line);
    return out;
}

// The key order the `--dump-cli` flag printed in.
std::vector<std::string> Dump(const CliOptions& o) {
    std::vector<std::string> out;
    char buf[128];
    const auto s = [&](const char* k, const std::string& v) { out.push_back(std::string(k) + "=" + v); };
    const auto p = [&](const char* k, const std::filesystem::path& v) { s(k, io::PathToUtf8(v)); };
    const auto n = [&](const char* k, long long v) { s(k, std::to_string(v)); };
    const auto f = [&](const char* k, double v) {
        std::snprintf(buf, sizeof(buf), "%.9g", v);
        s(k, buf);
    };
#if defined(_WIN32)
    const gfx::GfxApi platformBackend = gfx::GfxApi::D3D12;
#else
    const gfx::GfxApi platformBackend = gfx::GfxApi::Vulkan;
#endif
    const DrawTraceOptions& dt = o.drawTraceOptions;
    const AnimScenario& an = dt.anim;
    const CaptureCliOptions& c = o.capture;
    n("backend", static_cast<long long>(o.backend.value_or(platformBackend)));
    n("backendFromCli", o.backend.has_value());
    s("wgpuBackend", o.wgpuBackend);
    n("headlessTest", o.headlessTest);
    n("multiSceneTest", o.multiSceneTest);
    n("particleDiff", o.particleDiff);
    n("childModelCheck", o.childModelCheck);
    n("particleDiffDevice", o.particleDiffOptions.useDevice);
    n("particleDiffCurveTol", o.particleDiffOptions.curveTolerance);
    n("particleTraceDump", o.particleDiffOptions.dump);
    n("particleDiffFrames", o.traceFrames);
    s("particleTraceRecord", o.particleDiffOptions.recordPath);
    s("particleTraceCheck", o.particleDiffOptions.checkPath);
    n("drawTrace", o.drawTrace);
    n("drawTraceHd", dt.hd);
    n("drawTraceSdHdr", dt.sdHdr);
    n("drawTraceDebugView", dt.debugView);
    n("drawTraceSelect", dt.selectStride);
    n("drawTraceUnlit", dt.unlitOddGeosets);
    n("noClothDeform", dt.noClothDeform);
    n("drawTraceNoRefraction", dt.noRefraction);
    n("drawTraceNoDistortion", dt.noDistortion);
    n("drawTraceDistortionBuffer", dt.distortionBuffer);
    n("drawTraceNoMultiTex", dt.noMultiTex);
    n("drawTraceRefractionMask", dt.refractionMask);
    n("drawTraceDebugLight", dt.debugLight);
    n("drawTraceShadows", dt.shadows);
    n("drawTraceFog", dt.fogMode);
    n("drawTraceLazyAnim", dt.lazyAnim);
    n("drawTraceAllowLate", dt.allowLateAssets);
    s("drawTraceRecord", dt.recordPath);
    s("drawTraceCheck", dt.checkPath);
    s("drawTraceGolden", dt.goldenPath);
    f("drawTraceDistanceTol", dt.distanceTol);
    n("drawTraceCameraDistance", dt.cameraDistance);
    f("drawTraceCameraYaw", dt.cameraYaw);
    n("drawTracePerturb", dt.perturbSeed);
    n("drawTraceInstances", dt.instances);
    s("anim.sequence", an.sequence);
    n("anim.switchFrame", an.switchFrame);
    s("anim.switchSequence", an.switchSequence);
    n("anim.layerFrame", an.layerFrame);
    s("anim.layerSequence", an.layerSequence);
    n("anim.layerBlendInMs", an.layerBlendInMs);
    f("anim.layerWeight", an.layerWeight);
    n("anim.layerSubtrack", an.layerSubtrack);
    n("anim.noGlobals", an.noGlobals);
    n("anim.list", an.list);
    n("anim.particleList", an.particleList);
    n("anim.attachList", an.attachList);
    n("anim.ribbonList", an.ribbonList);
    n("anim.solvers", an.solvers);
    f("anim.groundZ", an.groundZ);
    n("anim.hasAim", an.hasAim);
    f("anim.aim.x", an.aim.x);
    f("anim.aim.y", an.aim.y);
    f("anim.aim.z", an.aim.z);
    n("anim.ragdoll", an.ragdoll);
    for (const auto& [slot, name] : an.d3Equip)
        s("anim.d3Equip", std::to_string(slot) + ":" + name);
    for (const auto& [slot, dye] : an.d3Dyes)
        s("anim.d3Dye", std::to_string(slot) + ":" + std::to_string(dye));
    n("anim.d3Sheathed", an.d3Sheathed);
    n("anim.probe", an.probe);
    s("listfilePath", o.listfilePath);
    s("traceGame", dt.game);
    s("tactKeyPath", o.tactKeyPath);
    s("contentRoot", o.contentRoot);
    p("mdxPath", o.model);
    for (const auto& e : o.extraModels)
        p("extraPath", e);
    n("doExport", c.enabled);
    p("exportWemPath", o.convert.wemPath);
    p("exportMdxPath", o.convert.mdxPath);
    n("exportMdxProfile", static_cast<long long>(o.convert.mdx.profile));
    n("exportMdxTextures", o.convert.mdx.textures);
    p("exportM3Path", o.convert.m3Path);
    n("exportM3Profile", static_cast<long long>(o.convert.m3.profile));
    n("exportM3Textures", o.convert.m3.textures);
    n("exportM3ExactPasses", o.convert.m3.exactPasses);
    n("exportM3SharpenKey", o.convert.m3.sharpenTeamKey);
    n("exportM3War3ModTextures", o.convert.m3.war3ModTextures);
    n("exportM3Effects", o.convert.m3.effects);
    n("exportM3StandardRefs", o.convert.m3.standardRefs);
    p("exportGltfPath", o.convert.gltfPath);
    n("exportGltfTextures", o.convert.gltf.textures);
    p("saveM3Path", o.convert.saveM3Path);
    n("saveM3MergeAnims", o.convert.saveM3.mergeAnimations);
    n("saveM3Sc2", o.convert.saveM3.convertToSc2);
    n("saveM3Textures", o.convert.saveM3.textures);
    n("wemProfile", static_cast<long long>(o.wemProfile));
    n("exportSeq", c.sequence);
    for (const auto& clip : c.clips)
        s("exportClip", clip);
    n("exportDurationMs", c.durationMs);
    n("exportFill", static_cast<long long>(c.fill.value_or(ExportFill::LoopLast)));
    n("exportFillSet", c.fill.has_value());
    n("exportPreRollMs", c.preRollMs);
    n("exportBlendMs", c.blendMs);
    n("exportFrameStep", c.frameStep);
    n("exportOrbit", c.orbit);
    f("exportOrbitDegPerSec", c.orbitDegPerSec);
    f("exportOrbitRevs", c.orbitRevolutions.value_or(0.0f));
    n("exportOrbitRevsSet", c.orbitRevolutions.has_value());
    f("exportOrbitPitch", c.orbitPitch.value_or(0.0f));
    n("exportOrbitPitchSet", c.orbitPitch.has_value());
    f("exportOrbitDist", c.orbitDistance);
    f("exportOrbitStart", c.orbitStart);
    n("exportOrbitFit", c.orbitFit);
    n("exportSubject", static_cast<long long>(c.subject));
    n("exportAngles", c.angles);
    n("exportSheetCols", c.sheetColumns);
    n("exportCrop", c.crop);
    s("exportNameTemplate", c.nameTemplate);
    p("exportRecipeFile", c.recipeFile);
    for (const auto& a : o.attachAnims)
        p("attachAnim", a);
    n("listClips", o.listClips);
    n("exportFps", c.fps.value_or(30));
    n("exportFpsSet", c.fps.has_value());
    n("exportFmt", static_cast<long long>(c.format.value_or(ExportFormat::PngFrames)));
    n("exportFmtSet", c.format.has_value());
    n("exportTransparent", c.transparent.value_or(false));
    n("exportTransparentSet", c.transparent.has_value());
    n("exportCaptureUi", c.captureUi.value_or(false));
    n("exportUiSet", c.captureUi.has_value());
    n("showCollisions", o.showCollisions);
    n("exportResW", c.resolution ? c.resolution->first : 0);
    n("exportResH", c.resolution ? c.resolution->second : 0);
    n("exportResSet", c.resolution.has_value());
    n("exportCamera", c.camera);
    p("exportFolder", c.folder);
    p("uiShot.out", o.uiShot.out);
    s("uiShot.panel", o.uiShot.panel);
    return out;
}

bool IsHelp(const Case& c) {
    return !c.lines.empty() && c.lines.front().rfind("Usage:", 0) == 0;
}

bool EndsWith(std::string_view s, std::string_view suffix) {
    return s.size() >= suffix.size() && s.substr(s.size() - suffix.size()) == suffix;
}

// The inputs the old parser accepted silently and strict parsing (P2.8) rejects:
// a flag with no value or too few, a value that is the next flag, a non-number
// where a number goes. Nothing else in the record may change.
bool RejectedSinceStrictParsing(const Case& c) {
    for (std::string_view suffix : {"-missing", "-short", "-swallows", "-nan"})
        if (EndsWith(c.name, suffix))
            return true;
    return c.name == "export-sheet-x" || c.name == "d3-dye-head=abc" || c.name == "d3-dye-torso=";
}

} // namespace

TEST_CASE("The option table parses every recorded argv the way the old parser did",
          "[viewer][cli]") {
    const std::vector<Case> cases = LoadCases();
    REQUIRE(cases.size() > 400);
    REQUIRE(cases.front().name == "no-args");
    REQUIRE(cases.front().dump);
    const std::set<std::string> defaults(cases.front().lines.begin(), cases.front().lines.end());

    usize rejected = 0;
    for (const Case& c : cases) {
        INFO("case " << c.name);
        const ParseResult r = Parse(c.argv);
        const i32 exit = r.exitCode.value_or(0);

        if (IsHelp(c)) {
            // The help text is generated from the table now (B6), so only its
            // shape is compared.
            CHECK(exit == c.exit);
            REQUIRE(r.exitCode.has_value());
            CHECK(r.message.rfind("Usage:", 0) == 0);
            continue;
        }
        if (RejectedSinceStrictParsing(c)) {
            ++rejected;
            CHECK(exit == kUsageExitCode);
            CHECK(r.messageIsError);
            // The message names the flag the user typed.
            const std::string flag = c.argv[c.argv[0].rfind("-", 0) == 0 ? 0 : 1];
            CHECK(r.message.rfind("[viewer] " + flag + ":", 0) == 0);
            continue;
        }
        CHECK(exit == c.exit);

        std::vector<std::string> produced = SplitLines(r.warnings);
        for (std::string& line : SplitLines(r.message))
            produced.push_back(std::move(line));
        if (!r.exitCode) {
            for (std::string& line : Dump(r.options))
                produced.push_back(std::move(line));
        }
        // The first block holds the whole no-argument dump the others differ from.
        if (c.dump && &c != &cases.front()) {
            std::vector<std::string> changed;
            for (const std::string& line : produced)
                if (!defaults.count(line))
                    changed.push_back(line);
            CHECK(changed == c.lines);
        } else {
            CHECK(produced == c.lines);
        }
    }
    // Every flag with a value has its three rejection cases, so a near-zero count
    // means the names drifted rather than the parser getting lenient again.
    CHECK(rejected > 100);
}

TEST_CASE("Strict parsing keeps negative numbers and names the flag", "[viewer][cli]") {
    const ParseResult aim = Parse(std::vector<std::string>{"m.m3", "--draw-trace-aim", "-12", "-0.5", "+40"});
    REQUIRE_FALSE(aim.exitCode);
    CHECK(aim.options.drawTraceOptions.anim.aim.x == -12.0f);
    CHECK(aim.options.drawTraceOptions.anim.aim.z == 40.0f);

    const ParseResult alias = Parse(std::vector<std::string>{"m.mdx", "-b"});
    REQUIRE(alias.exitCode == kUsageExitCode);
    CHECK(alias.message.rfind("[viewer] -b: expected <name>", 0) == 0);

    const ParseResult sheet = Parse(std::vector<std::string>{"m.mdx", "--export-sheet", "4x"});
    CHECK(sheet.exitCode == kUsageExitCode);
    const ParseResult frames = Parse(std::vector<std::string>{"m.mdx", "--trace-frames", "1.5"});
    CHECK(frames.exitCode == kUsageExitCode);
}

TEST_CASE("Run modes follow the old dispatch order", "[viewer][cli]") {
    const auto modeOf = [](std::vector<std::string> argv) {
        return SelectRunMode(Parse(argv).options);
    };
    CHECK(modeOf({"model.mdx"}) == RunMode::Interactive);
    CHECK(modeOf({"model.m3", "--attach-anim", "a.m3a"}) == RunMode::Interactive);
    CHECK(modeOf({"--headless-test", "--draw-trace", "m.mdx"}) == RunMode::HeadlessTest);
    CHECK(modeOf({"--particle-diff", "--draw-trace", "m.mdx"}) == RunMode::ParticleDiff);
    CHECK(modeOf({"--draw-trace", "m.mdx", "--list-clips"}) == RunMode::DrawTrace);
    CHECK(modeOf({"m.mdx", "--list-clips", "--export-wem", "o.wem"}) == RunMode::ListClips);
    CHECK(modeOf({"m.mdx", "--export-gltf", "o.glb", "--save-m3", "o.m3"}) == RunMode::ExportGltf);
    CHECK(modeOf({"m.mdx", "--export-anim", "0", "30", "out"}) == RunMode::CaptureAnimation);
    CHECK(modeOf({"--ui-shot", "s.png"}) == RunMode::UiShot);
    CHECK(NeedsWindow(RunMode::Interactive));
    CHECK_FALSE(NeedsWindow(RunMode::ExportWem));
    CHECK_FALSE(NeedsWindow(RunMode::UiShot));
}

TEST_CASE("The help lists every option", "[viewer][cli]") {
    const std::string help = HelpText();
    for (const char* flag : {"--export-wem", "--draw-trace-anim-switch", "--particle-diff",
                             "--export-anim", "--d3-equip", "--ui-shot-panel", "-b"})
        CHECK(help.find(flag) != std::string::npos);
}
