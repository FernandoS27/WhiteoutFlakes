// ============================================================================
// The animation-export recipe and the schedule built from it.
//
// The schedule is a pure function of the frame index — which clip, whether it
// starts there, the exact clock advance, the camera yaw — so all the
// interesting arithmetic is testable without a device: clip boundaries,
// fill-to-duration, drift compensation, loop closure, and the ini round trip.
//
// Case 2 is the one that matters most. Actor::Advance truncates
// `dtSec * 1000 + 0.5` to integer milliseconds, so a constant dt = 1/fps
// accumulates integer error — nine frames short over thirty seconds at 30 fps,
// thirty-six frames long at 60. The schedule emits an exact per-frame delta
// instead; this is the regression test for that, and a constant 1/fps fails it.
//
// Device-free, so a G0 test. The ini cases redirect the settings file into the
// build tree first — the real location is a shared per-user config directory
// on Linux and macOS, and a test has no business rewriting the developer's
// settings.
// ============================================================================

#include <catch2/catch_test_macros.hpp>

#include "capture/export_ini.h"
#include "capture/export_recipe.h"
#include "settings_ini.h"
#include "whiteout/flakes/util/path_utf8.h"

#include <cmath>
#include <fstream>
#include <numeric>

using namespace whiteout::flakes;

namespace {

// A small model: two one-second looping sequences, one half-second one, and a
// static pose. Durations chosen so a 30 fps schedule lands on whole frames.
std::vector<SequenceInfo> SampleSequences() {
    std::vector<SequenceInfo> s;
    auto add = [&](const char* name, i32 startMs, i32 endMs, bool nonLooping = false) {
        SequenceInfo info;
        info.name = name;
        info.startMs = startMs;
        info.endMs = endMs;
        info.nonLooping = nonLooping;
        s.push_back(std::move(info));
    };
    add("Stand", 0, 1000);
    add("Walk", 1000, 2000);
    add("Attack", 2000, 2500);
    add("Death", 3000, 3000, true); // static pose
    add("Stand", 4000, 5000);       // deliberate duplicate name
    return s;
}

std::vector<std::string> SampleNames() {
    std::vector<std::string> names;
    for (const SequenceInfo& s : SampleSequences())
        names.push_back(s.name);
    return names;
}

ExportClip Clip(i32 sequence, i32 repeats = 1, f32 speed = 1.0f) {
    ExportClip c;
    c.sequence = sequence;
    c.repeats = repeats;
    c.speed = speed;
    return c;
}

// Fresh ini per case, so one case's writes cannot satisfy another's reads.
struct ScopedIni {
    explicit ScopedIni(const char* name) {
        path = io::ExecutableDirectory() / name;
        std::error_code ec;
        std::filesystem::remove(path, ec);
        SetSettingsIniPathOverride(path);
    }
    ~ScopedIni() {
        SetSettingsIniPathOverride({});
    }
    std::filesystem::path path;
};

std::string FileText(const std::filesystem::path& p) {
    std::ifstream f(p);
    return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

} // namespace

// ---------------------------------------------------------------------------

TEST_CASE("A single clip keeps the original exporter's frame count", "[export]") {
    const auto seqs = SampleSequences();
    // The arithmetic transcribed from RunAnimationExport: one sequence,
    // round(durationMs * fps / 1000). This is what keeps a one-sequence export
    // bit-identical across the redesign.
    for (i32 fps : {24, 25, 30, 50, 60}) {
        for (i32 seq = 0; seq < 3; ++seq) {
            ExportRecipe r;
            r.timing.fps = fps;
            r.clips.push_back(Clip(seq));
            const ExportSchedule s = ExportSchedule::Build(r, seqs);
            const i32 durMs = seqs[static_cast<std::size_t>(seq)].endMs -
                              seqs[static_cast<std::size_t>(seq)].startMs;
            const i32 expected =
                std::max(1, static_cast<i32>(std::llround(static_cast<double>(durMs) * fps / 1000.0)));
            REQUIRE(s.FrameCount() == expected);
        }
    }
}

TEST_CASE("A static pose contributes exactly one frame", "[export]") {
    ExportRecipe r;
    r.clips.push_back(Clip(3)); // Death: startMs == endMs
    const ExportSchedule s = ExportSchedule::Build(r, SampleSequences());
    REQUIRE(s.FrameCount() == 1);
}

TEST_CASE("The accumulated clock is exact at every frame rate", "[export]") {
    // sum(dtMs) over frames 0..i must equal round(i * 1000 / fps). A constant
    // 1/fps fails this by 36 frames at 60 fps over thirty seconds, which is
    // the defect this whole mechanism exists for.
    const auto seqs = SampleSequences();
    for (i32 fps : {24, 25, 30, 48, 50, 60}) {
        ExportRecipe r;
        r.timing.fps = fps;
        r.timing.duration = ExportDuration::Fixed;
        r.timing.durationMs = 30000;
        r.clips.push_back(Clip(0));
        const ExportSchedule s = ExportSchedule::Build(r, seqs);
        REQUIRE(s.FrameCount() > 0);

        i32 clock = 0;
        for (i32 i = 0; i < s.FrameCount(); ++i) {
            clock += s.At(i).dtMs;
            REQUIRE(clock == ExportSchedule::ClockAtFrame(i, fps));
        }
        // …and the whole recording lands on its nominal length.
        REQUIRE(clock == ExportSchedule::ClockAtFrame(s.FrameCount() - 1, fps));
    }
}

TEST_CASE("A pass starts with a zero step and every other frame advances",
          "[export]") {
    ExportRecipe r;
    r.clips.push_back(Clip(0));
    const ExportSchedule s = ExportSchedule::Build(r, SampleSequences());
    REQUIRE(s.At(0).dtMs == 0);
    REQUIRE(s.At(0).passStart);
    REQUIRE(s.At(0).clipStart);
    for (i32 i = 1; i < s.FrameCount(); ++i) {
        REQUIRE(s.At(i).dtMs > 0);
        REQUIRE_FALSE(s.At(i).passStart);
    }
}

TEST_CASE("Clip boundaries neither duplicate nor drop a frame", "[export]") {
    ExportRecipe r;
    r.timing.fps = 30;
    r.clips.push_back(Clip(0)); // 1000 ms -> 30 frames
    r.clips.push_back(Clip(1)); // 1000 ms -> 30 frames
    r.clips.push_back(Clip(2)); // 500 ms  -> 15 frames
    const ExportSchedule s = ExportSchedule::Build(r, SampleSequences());
    REQUIRE(s.FrameCount() == 75);

    // Exactly one clipStart per clip, at the right frame.
    std::vector<i32> starts;
    for (i32 i = 0; i < s.FrameCount(); ++i)
        if (s.At(i).clipStart)
            starts.push_back(i);
    REQUIRE(starts == std::vector<i32>{0, 30, 60});
    REQUIRE(s.At(29).clipIndex == 0);
    REQUIRE(s.At(30).clipIndex == 1);
    REQUIRE(s.At(30).sequence == 1);
}

TEST_CASE("Repeats are separate plays, and speed scales the frame count",
          "[export]") {
    const auto seqs = SampleSequences();
    {
        ExportRecipe r;
        r.timing.fps = 30;
        r.clips.push_back(Clip(0, /*repeats*/ 3));
        const ExportSchedule s = ExportSchedule::Build(r, seqs);
        REQUIRE(s.FrameCount() == 90);
        i32 startCount = 0;
        for (i32 i = 0; i < s.FrameCount(); ++i)
            startCount += s.At(i).clipStart ? 1 : 0;
        // Each repeat restarts the clip — that is the only way a non-looping
        // sequence repeats at all.
        REQUIRE(startCount == 3);
    }
    {
        ExportRecipe r;
        r.timing.fps = 30;
        r.clips.push_back(Clip(0, 1, /*speed*/ 0.5f));
        REQUIRE(ExportSchedule::Build(r, seqs).FrameCount() == 60);
        r.clips[0].speed = 2.0f;
        REQUIRE(ExportSchedule::Build(r, seqs).FrameCount() == 15);
    }
}

TEST_CASE("A hold pins the pose and keeps the clock running", "[export]") {
    ExportRecipe r;
    r.timing.fps = 30;
    ExportClip c = Clip(2); // Attack, 2000..2500
    c.holdMs = 500;
    r.clips.push_back(c);
    const ExportSchedule s = ExportSchedule::Build(r, SampleSequences());
    REQUIRE(s.FrameCount() == 30); // 15 play + 15 hold

    REQUIRE_FALSE(s.At(14).holding);
    REQUIRE(s.At(15).holding);
    // One millisecond short of the end: the playlist wraps a scrub through the
    // same windowing as playback, so pinning a looping clip at exactly `endMs`
    // lands on its first frame instead of its last.
    REQUIRE(s.At(15).holdFrameMs == 2499);
    // Speed 1 and a non-zero dt through the hold: the world keeps running, so
    // the children and the emitters keep going.
    REQUIRE(s.At(20).speed == 1.0f);
    REQUIRE(s.At(20).dtMs > 0);
}

TEST_CASE("A clip start carries the play's first frame", "[export]") {
    ExportRecipe r;
    r.timing.fps = 30;
    ExportClip c = Clip(1); // Walk, 1000..2000
    c.trimStartMs = 200;
    c.trimEndMs = 700;
    r.clips.push_back(c);
    const ExportSchedule s = ExportSchedule::Build(r, SampleSequences());
    REQUIRE(s.FrameCount() == 15); // 500 ms
    // The runner re-bases to this on every clip start: a no-op for a fresh
    // switch, the trim here, and the whole restart for a repeat of the
    // sequence already playing.
    REQUIRE(s.At(0).seekFrameMs == 1200);
    REQUIRE(s.At(1).seekFrameMs == -1);

    // …and an UNtrimmed clip carries its own start rather than -1.
    ExportRecipe plain;
    plain.timing.fps = 30;
    plain.clips.push_back(Clip(1));
    const ExportSchedule ps = ExportSchedule::Build(plain, SampleSequences());
    REQUIRE(ps.At(0).seekFrameMs == 1000);
    REQUIRE(ps.At(1).seekFrameMs == -1);
}

TEST_CASE("A fixed length shorter than the queue truncates and says which clip",
          "[export]") {
    ExportRecipe r;
    r.timing.fps = 30;
    r.timing.duration = ExportDuration::Fixed;
    r.timing.durationMs = 1500; // 45 frames of a 75-frame queue
    r.clips.push_back(Clip(0));
    r.clips.push_back(Clip(1));
    r.clips.push_back(Clip(2));
    const ExportSchedule s = ExportSchedule::Build(r, SampleSequences());
    REQUIRE(s.FrameCount() == 45);
    REQUIRE(s.TruncatedFromClip() == 2);
}

TEST_CASE("Each fill mode fills to the requested length", "[export]") {
    const auto seqs = SampleSequences();
    const auto build = [&](ExportFill fill) {
        ExportRecipe r;
        r.timing.fps = 30;
        r.timing.duration = ExportDuration::Fixed;
        r.timing.durationMs = 4000; // 120 frames over a 60-frame queue
        r.timing.fill = fill;
        r.clips.push_back(Clip(0)); // Stand, 30 frames
        r.clips.push_back(Clip(2)); // Attack, 15 frames
        return ExportSchedule::Build(r, seqs);
    };

    {
        const ExportSchedule s = build(ExportFill::HoldLast);
        REQUIRE(s.FrameCount() == 120);
        REQUIRE(s.FillStartFrame() == 45);
        REQUIRE(s.At(45).holding);
        REQUIRE(s.At(119).holding);
        REQUIRE(s.At(119).holdFrameMs == 2499);
    }
    {
        const ExportSchedule s = build(ExportFill::LoopLast);
        REQUIRE(s.FrameCount() == 120);
        // Everything past the queue is the last clip, repeatedly.
        for (i32 i = s.FillStartFrame(); i < s.FrameCount(); ++i)
            REQUIRE(s.At(i).clipIndex == 1);
        // …and the final repeat is truncated rather than overshooting.
        REQUIRE(s.At(119).sequence == 2);
    }
    {
        const ExportSchedule s = build(ExportFill::LoopQueue);
        REQUIRE(s.FrameCount() == 120);
        REQUIRE(s.At(45).clipIndex == 0); // the queue starts over
        REQUIRE(s.At(45).clipStart);
    }
}

TEST_CASE("Orbit yaw is a function of the frame index, and revolutions close",
          "[export]") {
    const auto seqs = SampleSequences();
    {
        ExportRecipe r;
        r.timing.fps = 30;
        r.camera.mode = ExportCameraMode::Orbit;
        r.camera.timing = OrbitTiming::Velocity;
        r.camera.degPerSec = 60.0f;
        r.camera.startYawDeg = 10.0f;
        r.clips.push_back(Clip(0));
        const ExportSchedule s = ExportSchedule::Build(r, seqs);
        for (i32 i = 0; i < s.FrameCount(); ++i) {
            const f32 want = 10.0f + 60.0f * (static_cast<f32>(i) / 30.0f);
            REQUIRE(std::fabs(s.At(i).yawDeg - want) < 1e-3f);
        }
    }
    {
        // Revolutions derive the rate from the length, which is the only way
        // the last frame lands one step short of the first.
        ExportRecipe r;
        r.timing.fps = 30;
        r.camera.mode = ExportCameraMode::Orbit;
        r.camera.timing = OrbitTiming::Revolutions;
        r.camera.revolutions = 2.0f;
        r.clips.push_back(Clip(0));
        const ExportSchedule s = ExportSchedule::Build(r, seqs);
        const f32 perFrame = s.At(1).yawDeg - s.At(0).yawDeg;
        const f32 closing = s.At(s.FrameCount() - 1).yawDeg + perFrame - s.At(0).yawDeg;
        REQUIRE(std::fabs(closing - 720.0f) < 0.01f);
    }
}

TEST_CASE("Multi-angle passes restart the clock and offset the yaw", "[export]") {
    ExportRecipe r;
    r.timing.fps = 30;
    r.camera.mode = ExportCameraMode::Orbit;
    r.camera.timing = OrbitTiming::Velocity;
    r.camera.degPerSec = 0.0f;
    r.camera.angleCount = 8;
    r.clips.push_back(Clip(0));
    const ExportSchedule s = ExportSchedule::Build(r, SampleSequences());
    REQUIRE(s.FrameCount() == 30);
    REQUIRE(s.TotalFrameCount() == 240);
    REQUIRE(s.At(30).angle == 1);
    REQUIRE(s.At(30).passStart);
    REQUIRE(s.At(30).dtMs == 0);
    REQUIRE(std::fabs(s.At(30).yawDeg - 45.0f) < 1e-3f);
    REQUIRE(std::fabs(s.At(210).yawDeg - 315.0f) < 1e-3f);
}

TEST_CASE("Frame step simulates every frame and captures every Nth", "[export]") {
    ExportRecipe r;
    r.timing.fps = 30;
    r.timing.frameStep = 3;
    r.clips.push_back(Clip(0));
    const ExportSchedule s = ExportSchedule::Build(r, SampleSequences());
    REQUIRE(s.FrameCount() == 30);        // the world still steps 30 times
    REQUIRE(s.CapturedFrameCount() == 10); // …and 10 frames are written
    REQUIRE(s.OutputFps() == 10);
    REQUIRE(s.At(0).capture);
    REQUIRE_FALSE(s.At(1).capture);
    REQUIRE(s.At(3).capture);
}

TEST_CASE("An unresolved clip contributes no frames and one warning", "[export]") {
    ExportRecipe r;
    ExportClip missing;
    missing.sequence = -1;
    missing.savedName = "Spell Channel";
    r.clips.push_back(missing);
    r.clips.push_back(Clip(0));
    const ExportSchedule s = ExportSchedule::Build(r, SampleSequences());
    REQUIRE(s.FrameCount() == 30);
    REQUIRE(s.Warnings().size() == 1);
    REQUIRE(s.Warnings()[0].find("Spell Channel") != std::string::npos);
}

// ---------------------------------------------------------------------------
// Naming
// ---------------------------------------------------------------------------

TEST_CASE("Name templates expand every token", "[export]") {
    NameTokens t;
    t.model = "footman";
    t.anim = "stand";
    t.clip = "attack";
    t.index = 7;
    t.clipFrame = 3;
    t.angle = 2;
    t.fps = 30;
    t.width = 1280;
    t.height = 960;
    t.date = "20260830";
    t.time = "142530";

    REQUIRE(ExpandNameTemplate("{model}_{anim}_{index:04}", t) == "footman_stand_0007");
    REQUIRE(ExpandNameTemplate("{clip}-{clipframe}-{angle:02}", t) == "attack-3-02");
    REQUIRE(ExpandNameTemplate("{w}x{h}@{fps}", t) == "1280x960@30");
    REQUIRE(ExpandNameTemplate("{date}_{time}", t) == "20260830_142530");
    REQUIRE(ExpandNameTemplate("plain", t) == "plain");

    // An unknown token is left literal — a filename with a visible {frameno}
    // in it is a bug the user can see; an empty one is a collision they cannot.
    std::vector<std::string> unknown;
    REQUIRE(ExpandNameTemplate("{model}_{frameno}", t, &unknown) == "footman_{frameno}");
    REQUIRE(unknown == std::vector<std::string>{"frameno"});

    // The defaults reproduce the original exporter's filenames.
    REQUIRE(std::string(DefaultNameTemplate(ExportFormat::PngFrames)) == "{model}_{anim}_{index:04}");
    REQUIRE(std::string(DefaultNameTemplate(ExportFormat::Webp)) == "{model}_{anim}");
}

TEST_CASE("Sequence keys disambiguate duplicate names", "[export]") {
    const auto names = SampleNames();
    // Two sequences are called "Stand", so both carry an occurrence; the
    // unique ones do not.
    REQUIRE(SequenceKey(names, 0) == "Stand#0");
    REQUIRE(SequenceKey(names, 4) == "Stand#1");
    REQUIRE(SequenceKey(names, 1) == "Walk");

    REQUIRE(ResolveSequenceKey(names, "Stand#1") == 4);
    REQUIRE(ResolveSequenceKey(names, "Walk") == 1);
    // A model with fewer copies falls back to the first occurrence…
    REQUIRE(ResolveSequenceKey(names, "Stand#7") == 0);
    // …and a whitespace-padded name still matches.
    REQUIRE(ResolveSequenceKey(names, "  Walk ") == 1);
    REQUIRE(ResolveSequenceKey(names, "Nothing") == -1);
}

// ---------------------------------------------------------------------------
// Persistence
// ---------------------------------------------------------------------------

TEST_CASE("A recipe round-trips through the ini", "[export]") {
    ScopedIni ini("export_roundtrip.ini");
    const auto names = SampleNames();

    ExportRecipe out;
    out.timing.fps = 60;
    out.timing.duration = ExportDuration::Fixed;
    out.timing.durationMs = 6000;
    out.timing.fill = ExportFill::LoopQueue;
    out.timing.preRollMs = 250;
    out.timing.frameStep = 2;
    out.camera.mode = ExportCameraMode::Orbit;
    out.camera.timing = OrbitTiming::Revolutions;
    out.camera.revolutions = 1.5f;
    out.camera.subject = OrbitSubject::Model;
    out.camera.fitToBounds = true;
    out.camera.angleCount = 8;
    out.output.format = ExportFormat::Webp;
    out.output.transparent = true;
    out.output.width = 1280;
    out.output.height = 960;
    out.output.autoCrop = true;
    out.output.nameTemplate = "{model}_{clip}_{index:04}";
    out.output.folder = std::filesystem::path("D:/renders");
    out.clips.push_back(Clip(4, 2, 0.5f)); // Stand#1
    out.clips.push_back(Clip(1));
    out.clips.back().holdMs = 400;

    SaveExportRecipe(out, names, std::filesystem::path("Footman.mdx"));

    ExportRecipe in;
    ExportRecipeLoadReport report;
    LoadExportRecipe(in, names, &report);

    REQUIRE(report.hadSection);
    REQUIRE(report.unresolvedClips.empty());
    REQUIRE(in.timing.fps == 60);
    REQUIRE(in.timing.duration == ExportDuration::Fixed);
    REQUIRE(in.timing.durationMs == 6000);
    REQUIRE(in.timing.fill == ExportFill::LoopQueue);
    REQUIRE(in.timing.preRollMs == 250);
    REQUIRE(in.timing.frameStep == 2);
    REQUIRE(in.camera.mode == ExportCameraMode::Orbit);
    REQUIRE(in.camera.timing == OrbitTiming::Revolutions);
    REQUIRE(in.camera.revolutions == 1.5f);
    REQUIRE(in.camera.subject == OrbitSubject::Model);
    REQUIRE(in.camera.fitToBounds);
    REQUIRE(in.camera.angleCount == 8);
    REQUIRE(in.output.format == ExportFormat::Webp);
    REQUIRE(in.output.transparent);
    REQUIRE(in.output.width == 1280);
    REQUIRE(in.output.height == 960);
    REQUIRE(in.output.autoCrop);
    REQUIRE(in.output.nameTemplate == "{model}_{clip}_{index:04}");
    REQUIRE(in.output.folder.generic_string() == "D:/renders");

    REQUIRE(in.clips.size() == 2);
    // The queue is keyed by NAME: indices are export order and mean nothing
    // across models.
    REQUIRE(in.clips[0].sequence == 4);
    REQUIRE(in.clips[0].repeats == 2);
    REQUIRE(in.clips[0].speed == 0.5f);
    REQUIRE(in.clips[1].sequence == 1);
    REQUIRE(in.clips[1].holdMs == 400);
}

TEST_CASE("Saving a shorter queue leaves no stale clip sections", "[export]") {
    ScopedIni ini("export_shrink.ini");
    const auto names = SampleNames();

    ExportRecipe five;
    for (i32 i = 0; i < 5; ++i)
        five.clips.push_back(Clip(i % 3));
    SaveExportRecipe(five, names, {});
    REQUIRE(FileText(ini.path).find("[Export.Clip4]") != std::string::npos);

    ExportRecipe two;
    two.clips.push_back(Clip(0));
    two.clips.push_back(Clip(1));
    SaveExportRecipe(two, names, {});

    // Without IniMap::RemovePrefix, Clip2..4 would survive here forever and a
    // later hand-edit of ClipCount would resurrect them.
    const std::string text = FileText(ini.path);
    REQUIRE(text.find("[Export.Clip1]") != std::string::npos);
    REQUIRE(text.find("[Export.Clip2]") == std::string::npos);
    REQUIRE(text.find("[Export.Clip4]") == std::string::npos);

    ExportRecipe back;
    LoadExportRecipe(back, names, nullptr);
    REQUIRE(back.clips.size() == 2);
}

TEST_CASE("A clip the model does not have survives as an unresolved row",
          "[export]") {
    ScopedIni ini("export_unresolved.ini");
    const auto names = SampleNames();

    ExportRecipe out;
    out.clips.push_back(Clip(0));
    out.clips.push_back(Clip(2));
    SaveExportRecipe(out, names, {});

    // A different model: no "Attack".
    const std::vector<std::string> other = {"Stand", "Walk"};
    ExportRecipe in;
    ExportRecipeLoadReport report;
    LoadExportRecipe(in, other, &report);

    REQUIRE(in.clips.size() == 2);
    REQUIRE(in.clips[0].sequence == 0);
    REQUIRE(in.clips[1].sequence == -1);
    REQUIRE_FALSE(in.clips[1].Resolved());
    REQUIRE(in.clips[1].savedName == "Attack");
    REQUIRE(report.unresolvedClips == std::vector<std::string>{"Attack"});

    // …and it round-trips again rather than being silently dropped.
    SaveExportRecipe(in, other, {});
    ExportRecipe again;
    LoadExportRecipe(again, other, nullptr);
    REQUIRE(again.clips.size() == 2);
    REQUIRE(again.clips[1].savedName == "Attack");
}

TEST_CASE("A recipe file is the same writer pointed somewhere else", "[export]") {
    ScopedIni ini("export_recipefile.ini");
    const auto names = SampleNames();
    const std::filesystem::path file = io::ExecutableDirectory() / "sample_recipe.ini";
    std::error_code ec;
    std::filesystem::remove(file, ec);

    ExportRecipe out;
    out.timing.fps = 24;
    out.output.format = ExportFormat::Gif;
    out.clips.push_back(Clip(1, 3));
    REQUIRE(WriteExportRecipeFile(file, out, names, {}));

    ExportRecipe in;
    REQUIRE(ReadExportRecipeFile(file, in, names, nullptr));
    REQUIRE(in.timing.fps == 24);
    REQUIRE(in.output.format == ExportFormat::Gif);
    REQUIRE(in.clips.size() == 1);
    REQUIRE(in.clips[0].sequence == 1);
    REQUIRE(in.clips[0].repeats == 3);

    // The settings ini is untouched by a recipe-file write.
    REQUIRE_FALSE(std::filesystem::exists(ini.path));
}

TEST_CASE("Format tokens round-trip and an unknown one keeps the default",
          "[export]") {
    for (i32 i = 0; i < kExportFormatCount; ++i) {
        const auto want = static_cast<ExportFormat>(i);
        ExportFormat got = ExportFormat::PngFrames;
        REQUIRE(ParseExportFormat(GetExportFormatInfo(want).iniName, got));
        REQUIRE(got == want);
    }
    ExportFormat keep = ExportFormat::Webp;
    REQUIRE_FALSE(ParseExportFormat("avif", keep));
    REQUIRE(keep == ExportFormat::Webp);
}
