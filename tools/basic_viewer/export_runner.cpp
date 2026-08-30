#include "export_runner.h"

#include "export_encoders.h"
#include "renderer/camera.h"
#include "renderer/frame_ticker.h"
#include "renderer/model/model_instance.h"
#include "renderer/render_pipeline.h"
#include "renderer/render_service.h"
#include "renderer/scene_manager.h"
#include "renderer/viewport.h"
#include "thumbnail_framing.h"
#include "whiteout/flakes/util/path_utf8.h"

#include <whiteout/textures/texture.h>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>

namespace whiteout::flakes {
namespace {

namespace fs = std::filesystem;
using renderer::Camera;
using renderer::SceneId;
using renderer::Viewport;
using whiteout::textures::PixelFormat;
using whiteout::textures::Texture;

constexpr f32 kPi = 3.14159265358979323846f;

f32 Deg2Rad(f32 deg) {
    return deg * (kPi / 180.0f);
}

// ---------------------------------------------------------------------------
// State guard
// ---------------------------------------------------------------------------
//
// Every mutation an export makes, snapshotted here and put back in the
// destructor. It is a guard rather than a hand-written ledger at the end of
// the function because the list only grows, and because a cancel or an early
// return must not leave the viewer with a frozen clock, a resized target or a
// camera stuck in a preset — which is exactly what the original exporter did
// on any path that returned early.

class ExportStateGuard {
public:
    ExportStateGuard(const ExportHost& host, renderer::RenderService& svc,
                     renderer::model::Actor& hero)
        : host_(host), svc_(svc), hero_(hero), clock_(svc.SceneAt(host.scene)) {
        savedSeq_ = hero.animation.ActiveSequenceIndex();
        savedTime_ = hero.animation.TimeMs();
        savedCursor_ = hero.cursor;
        savedSpeed_ = hero.playbackSpeed;
        savedWorld_ = hero.worldTransform;

        savedPlayback_ = clock_.GetPlaybackState();
        savedTimeScale_ = clock_.GetTimeScale();

        savedWidth_ = svc.Pipeline().Width();
        savedHeight_ = svc.Pipeline().Height();

        savedFlags_ = svc.Settings().GetDisplayFlags();
        savedBg_ = svc.Settings().BackgroundColorRaw();

        Camera& cam = svc.SceneAt(host.scene).Camera();
        savedYaw_ = cam.GetYaw();
        savedPitch_ = cam.GetPitch();
        savedDistance_ = cam.GetDistance();
        savedTarget_ = cam.GetTarget();
        savedCamPreset_ = host.currentCameraPreset ? host.currentCameraPreset() : -1;
    }

    ~ExportStateGuard() {
        if (resized_)
            svc_.Pipeline().ResizePrimaryTarget(savedWidth_, savedHeight_);

        svc_.Settings().SetDisplayFlags(savedFlags_);
        svc_.Settings().SetBackgroundColor(static_cast<u8>(savedBg_ & 0xFF),
                                           static_cast<u8>((savedBg_ >> 8) & 0xFF),
                                           static_cast<u8>((savedBg_ >> 16) & 0xFF));

        hero_.worldTransform = savedWorld_;
        hero_.playbackSpeed = savedSpeed_;
        hero_.animation.SetActiveSequenceIndex(savedSeq_);
        hero_.cursor = savedCursor_;
        // Re-base onto the restored clock, then scrub back to the frame the
        // viewer was on. The Advance in between is what materialises the
        // restarted play — the scrub re-bases a play and there is none until
        // then.
        hero_.animation.Playlist().Restart(savedCursor_.actorTimeMs);
        hero_.animation.Advance(savedCursor_.actorTimeMs, hero_.ignoreNonLooping);
        hero_.animation.Playlist().SetPrimaryTimeMs(savedTime_, savedCursor_.actorTimeMs,
                                                    hero_.animation.Sequences());
        hero_.animation.SetTimeMs(savedTime_);
        // The frame-0 Restart re-bases every play, the layered ones included,
        // so the Animation window's tracks have to be re-asserted even though
        // a clip switch on its own leaves them alone.
        if (host_.reassertAnimTracks)
            host_.reassertAnimTracks();

        clock_.SetPlaybackState(savedPlayback_);
        clock_.SetTimeScale(savedTimeScale_);

        if (movedCamera_) {
            if (savedCamPreset_ >= 0 && host_.activateCameraPreset) {
                // It was on a model camera, which poses the camera directly.
                // Re-selecting the preset is the only restore that means
                // anything; the orbital pose below would point it elsewhere.
                host_.activateCameraPreset(savedCamPreset_);
            } else {
                Camera& cam = svc_.SceneAt(host_.scene).Camera();
                cam.SetOrbitalMode();
                cam.SetTarget(savedTarget_);
                cam.SetYaw(savedYaw_);
                cam.SetPitch(savedPitch_);
                cam.SetDistance(savedDistance_);
            }
        }
    }

    void NoteResize() {
        resized_ = true;
    }
    void NoteCameraMoved() {
        movedCamera_ = true;
    }
    i32 SavedWidth() const {
        return savedWidth_;
    }
    i32 SavedHeight() const {
        return savedHeight_;
    }

private:
    const ExportHost& host_;
    renderer::RenderService& svc_;
    renderer::model::Actor& hero_;
    renderer::SceneManager& clock_;

    i32 savedSeq_ = 0, savedTime_ = 0;
    renderer::model::Actor::Cursor savedCursor_{};
    f32 savedSpeed_ = 1.0f;
    Matrix44f savedWorld_ = Matrix44f::identity();

    PlaybackState savedPlayback_{};
    f32 savedTimeScale_ = 1.0f;

    i32 savedWidth_ = 0, savedHeight_ = 0;
    bool resized_ = false;

    DisplayFlags savedFlags_{};
    u32 savedBg_ = 0;

    f32 savedYaw_ = 0.0f, savedPitch_ = 0.0f, savedDistance_ = 0.0f;
    Vector3f savedTarget_{};
    bool movedCamera_ = false;
    i32 savedCamPreset_ = -1;
};

// ---------------------------------------------------------------------------
// Keying
// ---------------------------------------------------------------------------

// Recovers a straight-alpha RGBA frame from two captures of the same pose —
// one over a black backdrop, one over white. For a pixel of coverage a and
// colour C: black = a*C, white = a*C + (1-a). So (white-black) = 1-a, and the
// un-premultiplied colour is black/a. Pipeline-agnostic — works for HD and SD,
// anti-aliased edges and translucency alike.
Texture KeyOutBackground(const std::vector<u8>& black, const std::vector<u8>& white, i32 w, i32 h) {
    auto tex = Texture::create2D(PixelFormat::RGBA8, static_cast<u32>(w), static_cast<u32>(h), 1);
    auto dst = tex.mipData(0);
    const usize px = static_cast<usize>(w) * static_cast<usize>(h);
    if (dst.size() < px * 4 || black.size() < px * 4 || white.size() < px * 4)
        return tex;
    for (usize p = 0; p < px; ++p) {
        const u8* cb = &black[p * 4];
        const u8* cw = &white[p * 4];
        const f32 uncovered =
            ((static_cast<f32>(cw[0]) - cb[0]) + (static_cast<f32>(cw[1]) - cb[1]) +
             (static_cast<f32>(cw[2]) - cb[2])) /
            (3.0f * 255.0f);
        const f32 a = std::clamp(1.0f - uncovered, 0.0f, 1.0f);
        u8* o = &dst[p * 4];
        for (i32 c = 0; c < 3; ++c) {
            const f32 v = (a > 1.0f / 255.0f) ? static_cast<f32>(cb[c]) / a : 0.0f;
            o[c] = static_cast<u8>(std::clamp(v, 0.0f, 255.0f));
        }
        o[3] = static_cast<u8>(std::clamp(a * 255.0f, 0.0f, 255.0f));
    }
    return tex;
}

Texture TextureFromRgba(const std::vector<u8>& rgba, i32 w, i32 h) {
    auto tex = Texture::create2D(PixelFormat::RGBA8, static_cast<u32>(w), static_cast<u32>(h), 1);
    auto dst = tex.mipData(0);
    if (dst.size() < rgba.size())
        return tex;
    std::memcpy(dst.data(), rgba.data(), rgba.size());
    // Stamp the frame opaque. The captured alpha is the scene target's own,
    // and that is not a coverage mask: every blended particle multiplies it
    // down, so a WoW model's smoke exports as a hole the shape of its quads.
    // This is the opaque capture path; the transparent export keys its own
    // alpha in KeyOutBackground.
    for (usize a = 3; a < dst.size(); a += 4)
        dst[a] = 0xFF;
    return tex;
}

// ---------------------------------------------------------------------------
// The clip driver — the half of the runner that is playback, not pixels
// ---------------------------------------------------------------------------

class ClipDriver {
public:
    ClipDriver(renderer::model::Actor& hero, std::vector<SequenceInfo> seqs)
        : hero_(hero), seqs_(std::move(seqs)) {}

    /// Runs BEFORE Scene::Update. SetActiveSequence is lazy — it is noticed
    /// inside Actor::Advance, which Scene::Update calls — so a clip switch
    /// applied where the camera hooks run would land one frame late.
    void BeginFrame(const ExportStep& step) {
        hero_.playbackSpeed = step.speed;
        if (step.passStart) {
            // A pass genuinely rewinds the clock, and a play remembers the
            // clock value it started at: without the Restart every play would
            // hold a start stamp in the future and the model would sit on its
            // first frame. This is the ONLY place Restart belongs — it
            // re-bases every play including the model's global loops, so
            // using it per clip would visibly restart an `.m3`'s always-on
            // overlay at every boundary.
            const i32 n = static_cast<i32>(seqs_.size());
            acked_ = n > 0 ? ((step.sequence % n) + n) % n : step.sequence;
            hero_.animation.SetActiveSequenceIndex(acked_);
            hero_.cursor = {};
            hero_.animation.Playlist().Restart(0);
        } else if (step.clipStart) {
            const i32 n = static_cast<i32>(seqs_.size());
            const i32 bounded = n > 0 ? ((step.sequence % n) + n) % n : step.sequence;
            if (bounded != acked_) {
                // The ordinary in-range switch, which is noticed by the next
                // Advance and whose hard cut erases only the play marked
                // primary — global loops and the Animation window's layered
                // plays survive it untouched. Restart would not: it re-bases
                // EVERY play, so an `.m3`'s always-playing overlay would
                // visibly restart at each clip boundary.
                //
                // The index stays IN RANGE. The design called for bumping the
                // raw index by a multiple of the sequence count so a repeat of
                // the same clip would also be noticed, and ClipPlaylist does
                // bound it — but FrameTicker builds the single-play pose
                // request from `ActiveSequenceIndex()` *raw*
                // ([frame_ticker.cpp:324]), so an out-of-range value reaches
                // the sampler and the model falls back to its bind pose. A
                // repeat is handled below instead.
                if (step.blendMs > 0) {
                    // Cross-fade through the documented path: flipping the
                    // transition policy for exactly one Advance is what an SC2
                    // sequence switch does, and it is the only route that
                    // marks the incoming play PRIMARY — a bare `Play()` would
                    // leave the scrub, the hold and TimeMs() all pointing at
                    // the outgoing clip. Restored immediately, so nothing
                    // outside this frame sees it and the Warcraft III / WoW
                    // hard-cut path is untouched.
                    savedPolicy_ = hero_.animation.Playlist().Policy();
                    TransitionPolicy p = savedPolicy_;
                    p.crossFade = true;
                    p.blendInMs = step.blendMs;
                    p.blendOutMs = step.blendMs;
                    hero_.animation.Playlist().SetTransitionPolicy(p);
                    blendThisFrame_ = true;
                }
                hero_.animation.SetActiveSequenceIndex(bounded);
                acked_ = bounded;
            }
            // A repeat of the sequence already playing changes no index, so
            // there is nothing for the playlist to notice. AfterUpdate's
            // re-base to the clip's first frame IS the restart — and it is the
            // same call the trim and the hold already make.
        }
    }

    /// Runs after Scene::Update noticed the switch — the one Advance the
    /// flipped policy was for.
    void EndFrame() {
        if (!blendThisFrame_)
            return;
        hero_.animation.Playlist().SetTransitionPolicy(savedPolicy_);
        blendThisFrame_ = false;
    }

    /// Runs between Scene::Update and FrameTicker::Tick, where a re-base can
    /// still change the pose this frame: Update materialises the play, Tick
    /// evaluates it.
    void AfterUpdate(const ExportStep& step) {
        const i32 now = hero_.cursor.actorTimeMs;
        if (step.holding) {
            // The hold is a scrub, not a pause. `playbackSpeed = 0` would stop
            // the actor clock, and FrameTicker threads that clock down the
            // tree — so the children and the PE1 emitters would freeze with
            // their ancestor and the dust would stop settling. Pinning the
            // pose while the clock runs is what "hold on the death pose"
            // actually means.
            Rebase(step.holdFrameMs, now);
        } else if (step.clipStart && step.seekFrameMs >= 0 && !blendThisFrame_) {
            // No-op for a fresh switch (the play already starts there), the
            // trim for a trimmed clip, and the restart for a repeat. Skipped
            // on a cross-fade frame, where re-basing the incoming play would
            // undo the blend the policy just set up.
            Rebase(step.seekFrameMs, now);
        }
    }

private:
    void Rebase(i32 frameMs, i32 nowMs) {
        hero_.animation.Playlist().SetPrimaryTimeMs(frameMs, nowMs, seqs_);
        // The scrub re-bases the play's start stamp; the clip span the pose
        // is built from was computed by the Advance just before it. Re-advance
        // at the same clock so this frame samples the pinned pose rather than
        // the next one.
        hero_.animation.Advance(nowMs, hero_.ignoreNonLooping);
    }

    renderer::model::Actor& hero_;
    std::vector<SequenceInfo> seqs_;
    /// @brief The bounded sequence the playlist has been asked for.
    i32 acked_ = -1;
    bool blendThisFrame_ = false;
    TransitionPolicy savedPolicy_;
};

// ---------------------------------------------------------------------------
// Camera
// ---------------------------------------------------------------------------

struct OrbitBase {
    Vector3f target{};
    f32 yawDeg = 0.0f;
    f32 pitchRad = 0.0f;
    f32 distance = 0.0f;
    bool clampedDistance = false;
    bool clampedPitch = false;
};

OrbitBase ResolveOrbitBase(const ExportRecipe& recipe, renderer::RenderService& svc, SceneId scene,
                           renderer::model::Actor* hero) {
    Camera& cam = svc.SceneAt(scene).Camera();
    OrbitBase base;

    if (recipe.camera.fitToBounds) {
        // The existing helper, not a new fit: it is already format-neutral,
        // reads the actor's bounds rather than the template's, and applies
        // worldScale and the source coordinate space. It fits `maxAxis`, a
        // single scalar for the whole model, so the framing is yaw-invariant
        // — which is why it runs ONCE here and never per frame. A per-frame
        // re-fit would breathe once per revolution.
        tools::FrameCameraToModel(cam, hero);
        const f32 margin = (recipe.camera.fitMargin > 0.01f) ? recipe.camera.fitMargin : 1.0f;
        cam.SetDistance(cam.GetDistance() * margin);
    }

    base.target = cam.GetTarget();
    base.yawDeg = cam.GetYaw() * (180.0f / kPi);
    base.pitchRad = cam.GetPitch();
    base.distance = cam.GetDistance();

    if (recipe.camera.overridePitch) {
        const f32 want = Deg2Rad(recipe.camera.pitchDeg);
        cam.SetPitch(want);
        base.pitchRad = cam.GetPitch();
        base.clampedPitch = std::fabs(base.pitchRad - want) > 1e-4f;
    }
    if (recipe.camera.overrideDistance && recipe.camera.distance > 0.0f) {
        cam.SetDistance(recipe.camera.distance);
        base.distance = cam.GetDistance();
        base.clampedDistance = std::fabs(base.distance - recipe.camera.distance) > 0.5f;
    }
    if (!recipe.camera.yawRelative)
        base.yawDeg = 0.0f;
    return base;
}

// ---------------------------------------------------------------------------
// Naming
// ---------------------------------------------------------------------------

struct Naming {
    std::string tmpl;
    NameTokens tokens;
    std::vector<std::string> clipNames;
    std::vector<std::string> unknown;
};

Naming BuildNaming(const ExportRecipe& recipe, const ExportHost& host,
                   const ExportSchedule& schedule, i32 width, i32 height) {
    Naming n;
    n.tmpl = recipe.output.nameTemplate.empty() ? DefaultNameTemplate(recipe.output.format)
                                                : recipe.output.nameTemplate;

    n.tokens.model = SanitizeExportName(io::PathToUtf8(host.modelPath.stem()));
    for (const ExportClip& c : recipe.clips) {
        const bool ok = c.Resolved() && c.sequence < static_cast<i32>(host.sequenceNames.size());
        n.clipNames.push_back(SanitizeExportName(
            ok ? host.sequenceNames[static_cast<usize>(c.sequence)] : c.savedName));
    }
    // One clip names itself; a queue is a queue. That keeps the single-clip
    // case producing exactly the filenames the original exporter did.
    i32 resolved = 0;
    std::string first = "queue";
    for (usize i = 0; i < recipe.clips.size(); ++i)
        if (recipe.clips[i].Resolved()) {
            if (resolved == 0)
                first = n.clipNames[i];
            ++resolved;
        }
    n.tokens.anim = (resolved == 1) ? first : std::string("queue");
    n.tokens.clip = first;
    n.tokens.fps = schedule.OutputFps();
    n.tokens.width = width;
    n.tokens.height = height;

    const std::time_t now = std::time(nullptr);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &now);
#else
    localtime_r(&now, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y%m%d", &tm);
    n.tokens.date = buf;
    std::strftime(buf, sizeof(buf), "%H%M%S", &tm);
    n.tokens.time = buf;
    return n;
}

// ---------------------------------------------------------------------------
// Sidecar
// ---------------------------------------------------------------------------

std::string JsonEscape(std::string_view s) {
    std::string out;
    for (char c : s) {
        if (c == '"' || c == '\\')
            out += '\\';
        if (static_cast<unsigned char>(c) < 0x20) {
            char buf[8];
            std::snprintf(buf, sizeof(buf), "\\u%04x", c);
            out += buf;
            continue;
        }
        out += c;
    }
    return out;
}

void WriteSidecar(const fs::path& file, const ExportRecipe& recipe, const ExportSchedule& schedule,
                  const ExportHost& host, const ExportSinkResult& sink, i32 width, i32 height) {
    std::ofstream f(file);
    if (!f)
        return;
    f << "{\n";
    f << "  \"model\": \"" << JsonEscape(io::PathToUtf8(host.modelPath)) << "\",\n";
    f << "  \"fps\": " << schedule.OutputFps() << ",\n";
    f << "  \"simulationFps\": " << schedule.Fps() << ",\n";
    f << "  \"frames\": " << sink.framesWritten << ",\n";
    f << "  \"durationMs\": " << schedule.DurationMs() << ",\n";
    f << "  \"width\": " << width << ",\n";
    f << "  \"height\": " << height << ",\n";
    f << "  \"transparent\": " << (recipe.output.transparent ? "true" : "false") << ",\n";
    f << "  \"angles\": " << schedule.AngleCount() << ",\n";
    if (sink.sheetColumns > 0) {
        f << "  \"sheet\": { \"columns\": " << sink.sheetColumns
          << ", \"rows\": " << sink.sheetRows << ", \"cellWidth\": " << sink.cellWidth
          << ", \"cellHeight\": " << sink.cellHeight << " },\n";
    }
    if (sink.cropWidth > 0) {
        f << "  \"crop\": { \"x\": " << sink.cropX << ", \"y\": " << sink.cropY
          << ", \"width\": " << sink.cropWidth << ", \"height\": " << sink.cropHeight << " },\n";
    }
    if (recipe.camera.mode == ExportCameraMode::Orbit) {
        f << "  \"camera\": { \"mode\": \"orbit\", \"subject\": \""
          << (recipe.camera.subject == OrbitSubject::Model ? "model" : "camera")
          << "\", \"degPerSec\": " << recipe.camera.degPerSec << " },\n";
    }
    // Per-clip frame ranges: what makes a sprite sheet usable downstream.
    f << "  \"clips\": [\n";
    const auto starts = schedule.SegmentStarts();
    const auto clips = schedule.SegmentClips();
    for (usize i = 0; i < clips.size(); ++i) {
        const i32 begin = starts[i];
        const i32 end = (i + 1 < starts.size()) ? starts[i + 1] : schedule.FrameCount();
        const i32 c = clips[i];
        std::string name = "(hold)";
        if (c >= 0 && c < static_cast<i32>(recipe.clips.size())) {
            const ExportClip& clip = recipe.clips[static_cast<usize>(c)];
            if (clip.Resolved() && clip.sequence < static_cast<i32>(host.sequenceNames.size()))
                name = host.sequenceNames[static_cast<usize>(clip.sequence)];
            else
                name = clip.savedName;
        }
        f << "    { \"name\": \"" << JsonEscape(name) << "\", \"firstFrame\": " << begin
          << ", \"frameCount\": " << (end - begin) << " }" << (i + 1 < clips.size() ? "," : "")
          << "\n";
    }
    f << "  ]\n}\n";
}

} // namespace

// ---------------------------------------------------------------------------
// The run
// ---------------------------------------------------------------------------

ExportReport RunExport(const ExportRecipe& recipe, const ExportHost& host) {
    ExportReport report;
    const auto t0 = std::chrono::steady_clock::now();

    if (!host.service || !host.hero || !host.hero->animation.HasSource()) {
        report.error = "no animated model loaded";
        return report;
    }
    renderer::RenderService& svc = *host.service;
    renderer::model::Actor& hero = *host.hero;

    const std::vector<SequenceInfo> seqs = hero.animation.Sequences();
    if (seqs.empty()) {
        report.error = "the model has no animation sequences";
        return report;
    }

    const ExportSchedule schedule = ExportSchedule::Build(recipe, seqs);
    for (const std::string& w : schedule.Warnings())
        std::fprintf(stderr, "[viewer] Export: %s\n", w.c_str());

    const i32 totalFrames = schedule.TotalFrameCount();
    if (totalFrames <= 0) {
        report.error = "the recipe schedules no frames";
        return report;
    }
    if (totalFrames > kMaxExportFrames) {
        report.error = "the recipe schedules " + std::to_string(totalFrames) +
                       " frames, past the " + std::to_string(kMaxExportFrames) + "-frame cap";
        return report;
    }
    report.framesRequested = schedule.CapturedFrameCount();

    std::string unavailable;
    if (!ExportFormatAvailable(recipe.output.format, &unavailable)) {
        report.error = unavailable;
        return report;
    }

    std::error_code ec;
    fs::create_directories(recipe.output.folder, ec);
    report.folder = recipe.output.folder;

    // ---- state the export owns for its duration ----
    ExportStateGuard guard(host, svc, hero);

    const bool customRes = recipe.output.width > 0 && recipe.output.height > 0 &&
                           (recipe.output.width != guard.SavedWidth() ||
                            recipe.output.height != guard.SavedHeight());
    if (customRes) {
        svc.Pipeline().ResizePrimaryTarget(recipe.output.width, recipe.output.height);
        guard.NoteResize();
    }
    const i32 width = svc.Pipeline().Width();
    const i32 height = svc.Pipeline().Height();

    if (recipe.output.hideOverlays) {
        // Exporting the grid by accident is the most common self-inflicted
        // re-export. Particles and ribbons stay on — those are content.
        DisplayFlags df = svc.Settings().GetDisplayFlags();
        df.showGrid = false;
        df.showCollisions = false;
        df.showLights = false;
        df.showEvents = false;
        svc.Settings().SetDisplayFlags(df);
    }
    if (recipe.output.overrideBackground && !recipe.output.transparent)
        svc.Settings().SetBackgroundColor(recipe.output.backgroundR, recipe.output.backgroundG,
                                          recipe.output.backgroundB);

    // The export drives the clock itself, so the transport state must not be
    // able to freeze or stretch it — same reason playbackSpeed is driven per
    // frame rather than left where the user had it.
    renderer::SceneManager& clock = svc.SceneAt(host.scene);
    clock.SetPlaybackState(PlaybackState::Playing);
    clock.SetTimeScale(1.0f);

    // ---- camera ----
    OrbitBase orbitBase;
    if (recipe.camera.mode == ExportCameraMode::Preset && recipe.camera.preset >= 0 &&
        host.activateCameraPreset) {
        host.activateCameraPreset(recipe.camera.preset);
        guard.NoteCameraMoved();
    } else if (recipe.camera.mode == ExportCameraMode::Orbit) {
        orbitBase = ResolveOrbitBase(recipe, svc, host.scene, &hero);
        guard.NoteCameraMoved();
        if (orbitBase.clampedDistance)
            std::fprintf(stderr,
                         "[viewer] Export: orbit distance clamped to %.0f (camera limit)\n",
                         orbitBase.distance);
        if (orbitBase.clampedPitch)
            std::fprintf(stderr, "[viewer] Export: orbit pitch clamped to %.1f deg\n",
                         orbitBase.pitchRad * 180.0f / kPi);
    }
    const bool orbitCamera = recipe.camera.mode == ExportCameraMode::Orbit &&
                             recipe.camera.subject == OrbitSubject::Camera;
    const bool orbitModel = recipe.camera.mode == ExportCameraMode::Orbit &&
                            recipe.camera.subject == OrbitSubject::Model;

    // ---- sink ----
    const Naming naming = BuildNaming(recipe, host, schedule, width, height);
    std::unique_ptr<IExportSink> sink = MakeExportSink(recipe.output);
    ExportSinkSetup setup;
    setup.format = recipe.output.format;
    setup.folder = recipe.output.folder;
    setup.fps = schedule.OutputFps();
    setup.transparent = recipe.output.transparent;
    setup.sheetColumns = recipe.output.sheetColumns;
    setup.expectedFrames = schedule.CapturedFrameCount();
    setup.framesPerPass = (schedule.AngleCount() > 1)
                              ? schedule.CapturedFrameCount() / schedule.AngleCount()
                              : 0;
    setup.log = [](const std::string& m) { std::fprintf(stderr, "[viewer] %s\n", m.c_str()); };
    {
        NameTokens base = naming.tokens;
        std::vector<std::string> unknown;
        setup.baseName = ExpandNameTemplate(naming.tmpl, base, &unknown);
        setup.frameName = [&naming](const ExportStep& step) {
            NameTokens t = naming.tokens;
            t.index = step.frameIndex;
            t.clipFrame = step.clipFrameIndex;
            t.angle = step.angle;
            if (step.clipIndex >= 0 && step.clipIndex < static_cast<i32>(naming.clipNames.size()))
                t.clip = naming.clipNames[static_cast<usize>(step.clipIndex)];
            return ExpandNameTemplate(naming.tmpl, t);
        };
        for (const std::string& u : unknown)
            std::fprintf(stderr, "[viewer] Export: unknown name token '{%s}' left literal\n",
                         u.c_str());
        if (NameTemplateCollides(naming.tmpl, recipe.output.format))
            std::fprintf(stderr,
                         "[viewer] Export: '%s' has no {index}, so frames will overwrite each "
                         "other\n",
                         naming.tmpl.c_str());
    }
    if (!sink->Begin(setup)) {
        ExportSinkResult r;
        sink->Finish(r);
        report.error = r.error.empty() ? "the output encoder could not start" : r.error;
        return report;
    }

    // ---- the loop ----
    auto& pipeline = svc.Pipeline();
    pipeline.EnableFrameCapture(true);
    if (auto* cp = svc.SceneAt(host.scene).ActiveContentProvider())
        cp->Pump();

    ClipDriver driver(hero, seqs);

    const auto buildFrame = [&] {
        if (recipe.output.captureUi && host.buildUiFrame)
            host.buildUiFrame();
        else if (host.buildEmptyFrame)
            host.buildEmptyFrame();
    };

    const auto applyCamera = [&](const ExportStep& step) {
        if (orbitCamera) {
            Camera& cam = svc.SceneAt(host.scene).Camera();
            cam.SetOrbitalMode();
            cam.SetTarget(orbitBase.target);
            cam.SetPitch(orbitBase.pitchRad);
            cam.SetDistance(orbitBase.distance);
            cam.SetYaw(Deg2Rad(orbitBase.yawDeg + step.yawDeg));
        } else if (host.applyCameraPreset) {
            // Not just in Preset mode: "keep the current view" has to keep
            // *tracking* a preset the user already activated, or the camera
            // freezes wherever the animator last put it. The hook is a no-op
            // when no preset is active, which is the free-camera case.
            host.applyCameraPreset();
        }
    };

    const auto stepWorld = [&](const ExportStep& step) {
        driver.BeginFrame(step);
        if (orbitModel) {
            // Rotating the world transform is *motion* as far as cloth and
            // ragdolls are concerned, so a cloaked model will show the cape
            // reacting to the spin. Camera is the default for that reason.
            hero.worldTransform = Matrix44f::rotation_z(Deg2Rad(step.yawDeg));
        }
        const f32 dt = static_cast<f32>(step.dtMs) / 1000.0f;
        svc.SceneAt(host.scene).Update(dt);
        driver.AfterUpdate(step);
        driver.EndFrame();
        svc.Ticker().Tick(svc.SceneAt(host.scene), dt);
        svc.SetActiveScene(host.scene); // Tick restored the default scene
        applyCamera(step);
        buildFrame();
    };

    // ---- pre-roll: warm the simulation without recording it ----
    if (recipe.timing.preRollMs > 0) {
        const i32 rollFrames = std::clamp(
            static_cast<i32>(std::llround(static_cast<f64>(recipe.timing.preRollMs) *
                                          schedule.Fps() / 1000.0)),
            0, 3600);
        ExportStep warm = schedule.At(0);
        for (i32 i = 0; i < rollFrames; ++i) {
            ExportStep s = warm;
            s.passStart = (i == 0);
            s.clipStart = (i == 0);
            s.dtMs = (i == 0) ? 0 : (ExportSchedule::ClockAtFrame(i, schedule.Fps()) -
                                     ExportSchedule::ClockAtFrame(i - 1, schedule.Fps()));
            stepWorld(s);
            // Render but do not present a capture: the world has to actually
            // step, and the particle stages run off the render tick.
            Viewport vp;
            vp.scene = host.scene;
            vp.target = host.target;
            vp.camera = &svc.SceneAt(host.scene).Camera();
            pipeline.RenderViewport(vp);
            pipeline.Present(host.target);
        }
    }

    struct Pending {
        ExportStep step;
        i32 ringSlot;
    };
    std::vector<Pending> pending;
    // A UI frame cannot be pipelined — the ImGui renderer uploads through one
    // shared vertex buffer, so it must be drained before the next overwrites
    // it. The transparent path renders each pose twice and keys the pair, so
    // it cannot pipeline either.
    const bool pipelineFrames = !recipe.output.captureUi && !recipe.output.transparent;
    const i32 ringSize = pipelineFrames ? pipeline.FrameCaptureRingSize() : 1;
    pending.reserve(static_cast<usize>(std::max(1, ringSize)));

    auto* dev = pipeline.Gfx();
    const auto drain = [&]() {
        if (pending.empty())
            return;
        if (dev)
            dev->WaitIdle(); // the batch's GPU work has now retired
        for (const Pending& p : pending) {
            std::vector<u8> rgba;
            i32 w = 0, h = 0;
            if (!pipeline.DownloadCaptureSlot(p.ringSlot, rgba, w, h) || w <= 0 || h <= 0)
                continue;
            sink->Accept(p.step, TextureFromRgba(rgba, w, h));
            ++report.framesCaptured;
        }
        pending.clear();
    };

    const auto renderPass = [&](u8 r, u8 g, u8 b, std::vector<u8>& out, i32& w, i32& h) -> bool {
        svc.Settings().SetBackgroundColor(r, g, b);
        Viewport vp;
        vp.scene = host.scene;
        vp.target = host.target;
        vp.camera = &svc.SceneAt(host.scene).Camera();
        pipeline.RenderViewport(vp);
        pipeline.Present(host.target);
        if (dev)
            dev->WaitIdle();
        const i32 slot = pipeline.LastCapturedSlot();
        return slot >= 0 && pipeline.DownloadCaptureSlot(slot, out, w, h) && w > 0 && h > 0;
    };

    i32 lastReported = -1;
    for (i32 i = 0; i < totalFrames; ++i) {
        // The export owns the frame loop, so this is the only thing keeping
        // the window alive — and it is what makes Esc a cancel for free.
        glfwPollEvents();
        if (host.window && glfwGetKey(host.window, GLFW_KEY_ESCAPE) == GLFW_PRESS) {
            report.cancelled = true;
            break;
        }

        const ExportStep step = schedule.At(i);
        stepWorld(step);

        if (step.capture) {
            if (recipe.output.transparent) {
                // Each frame is rendered twice at the same pose — black
                // backdrop, then white — and keyed into straight alpha.
                std::vector<u8> black, white;
                i32 bw = 0, bh = 0, ww = 0, wh = 0;
                if (renderPass(0, 0, 0, black, bw, bh) &&
                    renderPass(255, 255, 255, white, ww, wh) && bw == ww && bh == wh) {
                    sink->Accept(step, KeyOutBackground(black, white, bw, bh));
                    ++report.framesCaptured;
                }
            } else {
                Viewport vp;
                vp.scene = host.scene;
                vp.target = host.target;
                vp.camera = &svc.SceneAt(host.scene).Camera();
                pipeline.RenderViewport(vp);
                pipeline.Present(host.target);
                const i32 slot = pipeline.LastCapturedSlot();
                if (slot >= 0)
                    pending.push_back({step, slot});
                if (static_cast<i32>(pending.size()) >= ringSize)
                    drain();
            }
        } else {
            // A frame the step skips still has to be simulated, or the world
            // would advance at a different rate than the one the schedule
            // computed. Render it (the particle stages run off the render
            // tick) and throw the pixels away.
            Viewport vp;
            vp.scene = host.scene;
            vp.target = host.target;
            vp.camera = &svc.SceneAt(host.scene).Camera();
            pipeline.RenderViewport(vp);
            pipeline.Present(host.target);
        }

        // Progress goes where it is free. An in-viewport overlay would have to
        // be composited into the frame being captured — the ImGui pass lands
        // in the captured image — so it would cost a second render pass per
        // frame and still show up in the output.
        const i32 pct = (i * 100) / std::max(1, totalFrames);
        if (pct != lastReported && (pct % 2) == 0) {
            lastReported = pct;
            if (host.onProgress)
                host.onProgress(i, totalFrames);
            std::fprintf(stderr, "[viewer] Export: %d/%d (%d%%)\r", i, totalFrames, pct);
        }
    }
    drain();
    pipeline.EnableFrameCapture(false);
    std::fprintf(stderr, "\n");

    // ---- encode ----
    ExportSinkResult sinkResult;
    const bool ok = sink->Finish(sinkResult);
    report.files = sinkResult.files;
    report.error = sinkResult.error;
    report.ok = ok && sinkResult.framesWritten > 0;
    report.framesCaptured = sinkResult.framesWritten;

    if (report.ok && recipe.output.writeSidecar && !sinkResult.files.empty()) {
        fs::path side = sinkResult.files.front();
        side.replace_extension(".json");
        if (LayoutOf(recipe.output.format) == ExportLayout::Frames)
            side = recipe.output.folder / io::FsPathFromUtf8(setup.baseName + ".json");
        WriteSidecar(side, recipe, schedule, host, sinkResult, width, height);
    }

    for (const fs::path& f : report.files) {
        std::error_code sizeEc;
        const auto sz = fs::file_size(f, sizeEc);
        if (!sizeEc)
            report.outputBytes += sz;
    }
    report.elapsedSec = std::chrono::duration<f64>(std::chrono::steady_clock::now() - t0).count();
    return report;
}

// ---------------------------------------------------------------------------
// Preview / scrub
// ---------------------------------------------------------------------------

bool PreviewExportFrame(const ExportRecipe& recipe, const ExportHost& host, i32 frameIndex,
                        bool applyCamera) {
    if (!host.service || !host.hero || !host.hero->animation.HasSource())
        return false;
    renderer::RenderService& svc = *host.service;
    renderer::model::Actor& hero = *host.hero;
    const std::vector<SequenceInfo> seqs = hero.animation.Sequences();
    if (seqs.empty())
        return false;

    const ExportSchedule schedule = ExportSchedule::Build(recipe, seqs);
    if (schedule.TotalFrameCount() <= 0)
        return false;

    // Scrubbing is a pose, not a simulation: jump the clock straight to the
    // frame's exact time rather than replaying every step to it. The same
    // schedule answers both, which is the whole reason it is a pure function.
    const ExportStep step = schedule.At(frameIndex);
    const i32 local = step.frameIndex % schedule.FrameCount();
    const i32 clockMs = ExportSchedule::ClockAtFrame(local, schedule.Fps());

    hero.playbackSpeed = step.speed;
    hero.cursor = {};
    hero.animation.SetActiveSequenceIndex(step.sequence);
    hero.animation.Playlist().Restart(0);
    hero.cursor.actorTimeMs = clockMs;
    hero.animation.Advance(clockMs, hero.ignoreNonLooping);
    if (step.holding)
        hero.animation.Playlist().SetPrimaryTimeMs(step.holdFrameMs, clockMs, seqs);
    else if (step.seekFrameMs >= 0)
        hero.animation.Playlist().SetPrimaryTimeMs(step.seekFrameMs, clockMs, seqs);

    if (applyCamera && recipe.camera.mode == ExportCameraMode::Orbit &&
        recipe.camera.subject == OrbitSubject::Camera) {
        const OrbitBase base = ResolveOrbitBase(recipe, svc, host.scene, &hero);
        Camera& cam = svc.SceneAt(host.scene).Camera();
        cam.SetOrbitalMode();
        cam.SetTarget(base.target);
        cam.SetPitch(base.pitchRad);
        cam.SetDistance(base.distance);
        cam.SetYaw(Deg2Rad(base.yawDeg + step.yawDeg));
    }
    return true;
}

} // namespace whiteout::flakes
