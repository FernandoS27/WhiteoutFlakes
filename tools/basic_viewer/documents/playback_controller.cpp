#include "documents/playback_controller.h"

#include "app/viewer_tuning.h"
#include "documents/document_manager.h"
#include "renderer/animation/clip_playlist.h"
#include "renderer/camera.h"
#include "renderer/effects/splat_service.h"
#include "renderer/model/model_instance.h"
#include "renderer/model/model_template.h"
#include "renderer/render_service.h"
#include "renderer/scene_manager.h"
#include "string_util.h"
#include "thumbnail_framing.h"

#include <algorithm>
#include <iterator>

namespace whiteout::flakes {

namespace model = renderer::model;
using renderer::Camera;

PlaybackController::PlaybackController(renderer::RenderService& service, DocumentManager& documents)
    : service_(service), documents_(documents) {}

DocumentState& PlaybackController::State() {
    return documents_.ActiveState();
}

const DocumentState& PlaybackController::State() const {
    return documents_.ActiveState();
}

model::Actor* PlaybackController::FocusActor() const {
    return service_.Scene().Actors().Find(State().focusActor);
}

// ---- Transport ------------------------------------------------------------------
//
// On the ACTIVE document's scene, so each tab holds its own pause.

bool PlaybackController::IsPaused() const {
    return service_.SceneAt(documents_.ActiveScene()).IsPaused();
}

void PlaybackController::SetPaused(bool paused) {
    service_.SceneAt(documents_.ActiveScene())
        .SetPlaybackState(paused ? PlaybackState::Paused : PlaybackState::Playing);
}

void PlaybackController::RestartPlayback() {
    // RewindScene and the effect services it restarts resolve through the ACTIVE
    // scene, so publish this document's before asking.
    const renderer::SceneId scene = documents_.ActiveScene();
    service_.SetActiveScene(scene);
    service_.RewindScene();
    service_.SceneAt(scene).SetPlaybackState(PlaybackState::Playing);

    DocumentState& state = State();
    // The parent-clock delta is measured against this stamp; the pre-rewind
    // (larger) value would report one negative frame.
    state.lastParentTimeMs = 0;

    // Walk drift is an offset this host pushed the actor along by, not scene
    // state the rewind touches, so take it back off.
    if (state.walkDrift.accumulated != 0.0f) {
        if (auto* hero = FocusActor())
            hero->worldTransform.data[3][0] -= state.walkDrift.accumulated;
        Camera& cam = service_.SceneAt(scene).Camera();
        const auto t = cam.GetTarget();
        cam.SetTarget(t.x - state.walkDrift.accumulated, t.y, t.z);
        state.walkDrift.accumulated = 0.0f;
    }
}

// ---- Sequences ------------------------------------------------------------------

const std::vector<SequenceInfo>& PlaybackController::Sequences() const {
    return State().sequences;
}

const std::vector<std::string>& PlaybackController::SequenceNames() const {
    return State().sequenceNames;
}

i32 PlaybackController::ActiveSequence() const {
    const model::Actor* focus = FocusActor();
    return focus ? focus->animation.ActiveSequenceIndex() : 0;
}

void PlaybackController::SelectSequence(i32 index) {
    model::Actor* focus = FocusActor();
    if (!focus)
        return;
    const i32 previous = focus->animation.ActiveSequenceIndex();
    focus->animation.SetActiveSequenceIndex(index);
    if (index == previous || index < 0 || index >= static_cast<i32>(SequenceNames().size()))
        return;
    const std::string& name = SequenceNames()[static_cast<usize>(index)];
    const bool keepSplats = std::any_of(std::begin(tuning::kSplatKeepingSequenceTokens),
                                        std::end(tuning::kSplatKeepingSequenceTokens),
                                        [&](std::string_view t) { return name.find(t) != std::string::npos; });
    if (!keepSplats)
        service_.Splats().Clear();
}

void PlaybackController::RefreshSequences(DocumentState& state, model::Actor* hero, bool resetSelection) {
    if (!hero)
        return;
    state.sequences = hero->animation.Sequences();
    state.sequenceNames.clear();
    state.sequenceNames.reserve(state.sequences.size());
    for (const SequenceInfo& s : state.sequences)
        state.sequenceNames.push_back(s.name);

    if (resetSelection) {
        state.tracks.clear();
        state.silencedGlobals.clear();
    } else {
        ReassertTracks();
        PublishGlobalLoops();
    }

    if (state.sequences.empty())
        return;
    const i32 count = static_cast<i32>(state.sequences.size());
    const i32 current = hero->animation.ActiveSequenceIndex();
    if (resetSelection || current < 0 || current >= count)
        hero->animation.SetActiveSequenceIndex(0);
}

// ---- Layered plays --------------------------------------------------------------

const std::vector<AnimTrack>& PlaybackController::Tracks() const {
    return State().tracks;
}

bool PlaybackController::AddTrack() {
    model::Actor* hero = FocusActor();
    if (!hero || !hero->animation.HasSource())
        return false;
    DocumentState& state = State();
    AnimTrackInfo info;
    info.sequence = hero->animation.ActiveSequenceIndex();
    if (info.sequence < 0 || info.sequence >= static_cast<i32>(state.sequences.size()))
        info.sequence = 0;
    state.tracks.push_back({info, 0});
    SetTrack(state.tracks.size() - 1, info);
    return true;
}

void PlaybackController::SetTrack(usize index, const AnimTrackInfo& info) {
    DocumentState& state = State();
    if (index >= state.tracks.size())
        return;
    model::Actor* hero = FocusActor();
    if (!hero || !hero->animation.HasSource())
        return;
    auto& playlist = hero->animation.Playlist();
    AnimTrack& track = state.tracks[index];
    const AnimTrackInfo prev = track.info;
    track.info = info;

    const bool restart = prev.sequence != info.sequence || prev.subtrack != info.subtrack || track.handle == 0;
    if (!restart && playlist.Retune(track.handle, info.weight, info.speed, info.loop))
        return;

    // The animation itself changed, or the play is gone (it ran out, or a Bind
    // rebuilt the stack). Replace it outright.
    if (track.handle != 0)
        playlist.Stop(track.handle, 0, hero->cursor.actorTimeMs);
    renderer::animation::PlayDesc d;
    d.sequence = info.sequence;
    d.subtrack = info.subtrack.value_or(-1);
    d.weight = info.weight;
    d.speed = info.speed;
    d.loop = info.loop;
    // Persistent, like every host-started play: the covered-play cull exists to
    // drop what a sequence switch buried, and a track added by hand is not that.
    // It also keeps the dropdown working — the playlist treats a stack of
    // persistent plays as an empty foreground.
    d.persistent = true;
    track.handle = playlist.Play(d, hero->cursor.actorTimeMs);
}

void PlaybackController::RemoveTrack(usize index) {
    DocumentState& state = State();
    if (index >= state.tracks.size())
        return;
    if (model::Actor* hero = FocusActor(); hero && hero->animation.HasSource())
        hero->animation.Playlist().Stop(state.tracks[index].handle, 0, hero->cursor.actorTimeMs);
    state.tracks.erase(state.tracks.begin() + static_cast<std::ptrdiff_t>(index));
}

void PlaybackController::ReassertTracks() {
    DocumentState& state = State();
    for (AnimTrack& t : state.tracks)
        t.handle = 0; // forces SetTrack down its restart path
    for (usize i = 0; i < state.tracks.size(); ++i)
        SetTrack(i, AnimTrackInfo(state.tracks[i].info));
}

// ---- Global loops ---------------------------------------------------------------
//
// Sequences the model plays by itself, forever, over everything else —
// StarCraft II's `GLstand` / `GLbirth`. On by default because the engine does;
// switchable because a viewer is also for looking at one thing at a time.

std::vector<PlaybackController::GlobalLoop> PlaybackController::GlobalLoops() const {
    const DocumentState& state = State();
    std::vector<GlobalLoop> out;
    for (usize i = 0; i < state.sequences.size(); ++i) {
        if (!state.sequences[i].alwaysPlays)
            continue;
        const i32 seq = static_cast<i32>(i);
        const bool silenced = std::find(state.silencedGlobals.begin(), state.silencedGlobals.end(), seq) !=
                              state.silencedGlobals.end();
        out.push_back({seq, state.sequenceNames[i], !silenced});
    }
    return out;
}

void PlaybackController::SetGlobalLoopEnabled(i32 sequence, bool on) {
    auto& silenced = State().silencedGlobals;
    const auto it = std::find(silenced.begin(), silenced.end(), sequence);
    if (on == (it == silenced.end()))
        return;
    if (on)
        silenced.erase(it);
    else
        silenced.push_back(sequence);
    PublishGlobalLoops();
}

void PlaybackController::PublishGlobalLoops() {
    model::Actor* hero = FocusActor();
    if (!hero || !hero->animation.HasSource())
        return;
    // The whole set every time rather than a stop on one play: the playlist owns
    // the global plays and reconciles against this list, so a subset is the only
    // way to keep it from restarting the silenced one — and reconciling keeps
    // the survivors' clocks.
    const DocumentState& state = State();
    std::vector<i32> live;
    for (usize i = 0; i < state.sequences.size(); ++i) {
        if (!state.sequences[i].alwaysPlays)
            continue;
        const i32 seq = static_cast<i32>(i);
        if (std::find(state.silencedGlobals.begin(), state.silencedGlobals.end(), seq) ==
            state.silencedGlobals.end())
            live.push_back(seq);
    }
    hero->animation.Playlist().SetGlobalSequences(std::move(live));
}

// ---- Camera presets -------------------------------------------------------------

const std::vector<CameraPreset>& PlaybackController::CameraPresets() const {
    return State().camera.presets;
}

std::optional<i32> PlaybackController::ActiveCameraPreset() const {
    return State().camera.active;
}

bool PlaybackController::CameraLocked() const {
    return State().camera.locked;
}

namespace {

// Where an animated preset puts the camera at the focus actor's time, sampled
// against the active sequence's range (open-ended when it has none).
void SamplePresetPose(const CameraPreset& preset, const model::Actor* focus,
                      const std::vector<SequenceInfo>& sequences, i32 sceneTimeMs, Vector3f& pos,
                      Vector3f& target, f32& roll) {
    pos = preset.position;
    target = preset.target;
    roll = preset.staticRoll;
    if (!preset.animator)
        return;
    i32 seqStart = 0;
    i32 seqEnd = 0;
    const i32 seqIdx = focus ? focus->animation.ActiveSequenceIndex() : 0;
    if (seqIdx >= 0 && seqIdx < static_cast<i32>(sequences.size())) {
        seqStart = sequences[static_cast<usize>(seqIdx)].startMs;
        seqEnd = sequences[static_cast<usize>(seqIdx)].endMs;
    }
    if (seqStart == 0 && seqEnd == 0)
        seqEnd = tuning::kOpenEndedSequenceEndMs;
    const i32 sampleMs = focus ? focus->animation.TimeMs() : sceneTimeMs;
    preset.animator(pos, target, roll, sampleMs, seqStart, seqEnd);
}

} // namespace

void PlaybackController::ActivateCameraPreset(std::optional<i32> index) {
    CameraPresetState& camera = State().camera;
    auto& cam = service_.Scene().Camera();

    if (!index || *index < 0 || *index >= static_cast<i32>(camera.presets.size())) {
        camera.active.reset();
        camera.locked = false;
        cam.SetOrbitalMode();
        cam.SetFovDiagonal(Camera::kDefaultFovDiagonal);
        cam.SetClip(Camera::kDefaultNearZ, Camera::kDefaultFarZ);
        return;
    }

    camera.active = *index;
    const CameraPreset& preset = camera.presets[static_cast<usize>(*index)];
    camera.locked = preset.isLive;

    Vector3f pos;
    Vector3f tgt;
    f32 roll = 0.0f;
    SamplePresetPose(preset, FocusActor(), State().sequences, service_.Scene().GetAnimationTime(), pos,
                     tgt, roll);
    cam.SetDirectPose(pos, tgt, roll);
    cam.SetFovDiagonal(preset.fovDiagonal > tuning::kPresetFovUnset ? preset.fovDiagonal
                                                                   : Camera::kDefaultFovDiagonal);
    cam.SetClip(preset.zNear, preset.zFar);
}

void PlaybackController::UpdateCameraPresetAnimator() {
    const DocumentState& state = State();
    const auto& active = state.camera.active;
    if (!active || *active < 0 || *active >= static_cast<i32>(state.camera.presets.size()))
        return;
    const CameraPreset& preset = state.camera.presets[static_cast<usize>(*active)];
    if (!preset.animator)
        return;
    Vector3f pos;
    Vector3f tgt;
    f32 roll = 0.0f;
    SamplePresetPose(preset, FocusActor(), state.sequences, service_.Scene().GetAnimationTime(), pos, tgt,
                     roll);
    service_.Scene().Camera().SetDirectPose(pos, tgt, roll);
}

// ---- Host policy ----------------------------------------------------------------

void PlaybackController::SetLoopNonLooping(bool on) {
    loopNonLooping_ = on;
    for (auto& [h, mi] : service_.Scene().Actors().All()) {
        if (mi->IsChild())
            continue;
        mi->ignoreNonLooping = on;
    }
}

// ---- Load -----------------------------------------------------------------------

void PlaybackController::OnModelLoaded(DocumentState& state, model::Actor* hero,
                                       const std::filesystem::path& path) {
    if (!hero)
        return;
    state.focusActor = hero->handle;
    hero->ignoreNonLooping = loopNonLooping_;

    RefreshSequences(state, hero, /*resetSelection*/ true);
    tools::FrameCameraToModel(service_.Scene().Camera(), hero);

    state.camera = {};
    if (hero->sourceTemplate)
        state.camera.presets = hero->sourceTemplate->cameraPresets;
    state.walkDrift = {};
    state.modelPath = path;
}

void PlaybackController::OnEffectLoaded(DocumentState& state, model::Actor* hero) {
    if (!hero)
        return;
    state.focusActor = hero->handle;
    hero->ignoreNonLooping = loopNonLooping_;

    // The source exposes one placeholder "Effect" sequence; mirrored like a
    // model's so the sequence picker stays consistent.
    state.sequences = hero->animation.Sequences();
    state.sequenceNames.clear();
    for (const SequenceInfo& s : state.sequences)
        state.sequenceNames.push_back(s.name);
    if (!state.sequences.empty())
        hero->animation.SetActiveSequenceIndex(0);

    // No mesh bounds, and the PopcornFX extents are unknown until the sim has
    // run a few frames: a provisional pose now, the real framing once the cloud
    // develops (AdvanceEffectReframe).
    auto& cam = service_.Scene().Camera();
    cam.SetOrbitalMode();
    cam.SetTarget(Vector3f{0.0f, 0.0f, 0.0f});
    cam.SetYaw(Camera::kDefaultYaw - tuning::kEffectPreviewYawOffset);
    cam.SetPitch(tuning::kEffectPreviewPitch);
    cam.SetDistance(tuning::kEffectPreviewDistance);
    cam.SetFovDiagonal(Camera::kDefaultFovDiagonal);
    cam.SetClip(Camera::kDefaultNearZ, Camera::kDefaultFarZ);
    state.effectReframeTicks = 0;

    // Effects carry no camera presets.
    state.camera = {};
    state.walkDrift = {};
}

// ---- Frame stages ---------------------------------------------------------------

void PlaybackController::AdvanceWalkDrift(f32 dt) {
    auto* hero = FocusActor();
    auto& scene = service_.Scene();
    if (!hero || scene.Camera().GetMode() != Camera::Mode::Orbital)
        return;
    DocumentState& state = State();
    const i32 idx = hero->animation.ActiveSequenceIndex();
    f32 delta = 0.0f;
    if (idx != state.walkDrift.prevSequence) {
        delta = -state.walkDrift.accumulated;
        state.walkDrift.prevSequence = idx;
    } else if (idx >= 0 && idx < static_cast<i32>(state.sequences.size())) {
        const SequenceInfo& s = state.sequences[static_cast<usize>(idx)];
        if (tools::ContainsIgnoreCase(s.name, tuning::kWalkSequenceToken)) {
            const f32 speed = s.moveSpeed != 0.0f ? s.moveSpeed : tuning::kDefaultWalkSpeed;
            // The scene's dt, not the frame's: this drift IS the walk cycle moving
            // the model, so a pause stops it too. The unwind above stays on the raw
            // path — switching sequence while paused still snaps the model back.
            delta = speed * scene.EffectiveDt(dt);
        }
    }
    if (delta != 0.0f) {
        state.walkDrift.accumulated += delta;
        hero->worldTransform.data[3][0] += delta;
        const auto t = scene.Camera().GetTarget();
        scene.Camera().SetTarget(t.x + delta, t.y, t.z);
    }
}

f32 PlaybackController::ParentClockStep() {
    DocumentState& state = State();
    const i32 now = service_.Scene().GetAnimationTime();
    const i32 stepMs = std::clamp(now - state.lastParentTimeMs, 0, tuning::kMaxParentClockStepMs);
    state.lastParentTimeMs = now;
    return static_cast<f32>(stepMs) / 1000.0f;
}

void PlaybackController::AdvanceEffectReframe() {
    // A short warm-up so the bounds reflect the steady-state spread, then retry
    // until particles exist (some effects spawn on a delay), giving up after a
    // bounded window.
    auto& ticks = State().effectReframeTicks;
    if (!ticks)
        return;
    ++*ticks;
    if (*ticks < tuning::kEffectReframeWarmupTicks)
        return;
    if (tools::FrameCameraToEffect(service_, service_.Scene().Camera(), State().focusActor) ||
        *ticks >= tuning::kEffectReframeMaxTicks)
        ticks.reset();
}

} // namespace whiteout::flakes
