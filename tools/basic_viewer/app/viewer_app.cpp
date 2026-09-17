#include "app/viewer_app.h"

#include "app/viewer_tuning.h"
#include "io/file_content_provider.h"
#include "storage_explorer.h"
#include "ui/ui_metrics.h"
#include "ui/viewer_ui.h"

#include "renderer/assets/replaceable_texture_manager.h"
#include "renderer/camera.h"
#include "renderer/frame_ticker.h"
#include "renderer/model/model_instance.h"
#include "renderer/model/model_loader.h"
#include "renderer/render_pipeline.h"
#include "renderer/render_service.h"
#include "renderer/scene_manager.h"
#include "renderer/viewport.h"
#include "whiteout/flakes/content_provider.h"
#include "whiteout/flakes/util/path_utf8.h"

#include <imgui.h>

#include <algorithm>
#include <cstdio>
#include <functional>
#include <string>
#include <system_error>
#include <vector>

namespace whiteout::flakes {

using namespace whiteout::flakes::renderer;
using namespace whiteout::flakes::renderer::model;

ViewerApp::ViewerApp(RenderService& service)
    : service_(service),
      window_(service),
      session_(service, tasks_),
      documents_(service),
      playback_(service, documents_),
      loader_(service, documents_, playback_, session_),
      features_(service, tasks_, documents_, playback_, loader_),
      exports_(service, documents_, playback_),
      capture_(service, window_, documents_, playback_),
      cameraInput_(service, playback_),
      explorer_(service, tasks_, session_,
                [this](const tools::ActivatedFile& f) { loader_.OpenStorageDocument(f.path, f.isEffect, f.provider); }) {
    // A different Diablo III install means a different item registry.
    session_.SetOnProfileApplying([this](ProductId game) {
        if (auto* d3 = features_.D3(); d3 && game == ProductId::D3)
            d3->OnProfileApplying();
    });
    // The client databases landed behind a model spawned before them.
    session_.SetOnWowTablesReady([this] {
        if (auto* wow = features_.Wow())
            wow->RestyleActiveModel();
    });
    ui_ = std::make_unique<ViewerUI>(*this);
    capture_.SetUiFrameBuilder([this] { ui_->BuildFrame(); });
}

ViewerApp::~ViewerApp() {
    Close();
}

bool ViewerApp::Open(i32 width, i32 height, gfx::GfxApi api, bool visible) {
    WindowEvents events;
    // dt 0 — the modal sizing loop owns wall-clock time; advancing animation per
    // repaint would fast-forward the scene while the user drags.
    events.redraw = [this] { Tick(0.0f); };
    events.mouseButton = [this](i32 button, i32 action, f64 x, f64 y) {
        cameraInput_.OnMouseButton(button, action, x, y);
    };
    events.cursorPos = [this](f64 x, f64 y) { cameraInput_.OnCursorPos(x, y); };
    events.scroll = [this](f64 yoffset) { cameraInput_.OnScroll(yoffset); };
    return window_.Open(width, height, api, visible, dpiScaleOverride_, std::move(events));
}

void ViewerApp::Close() {
    explorer_.Shutdown();
    window_.Close();
}

bool ViewerApp::ShouldClose() const {
    return window_.ShouldClose();
}

// ---- Tabs ------------------------------------------------------------------------

void ViewerApp::ActivateDocument(i32 index) {
    if (!documents_.Activate(index))
        return;
    loader_.ApplyRenderMode(service_.EffectiveRenderMode(service_.SceneAt(documents_.ActiveScene())));
}

void ViewerApp::CloseDocument(i32 index) {
    if (documents_.Close(index))
        loader_.ApplyRenderMode(service_.EffectiveRenderMode(service_.SceneAt(documents_.ActiveScene())));
}

// ---- Frame -----------------------------------------------------------------------

void ViewerApp::Tick(f32 dt) {
    // The active document's scene BEFORE polling: input callbacks fire inside
    // the poll and steer the active scene's camera, and every Scene() read below
    // must resolve to the active document too.
    service_.SetActiveScene(documents_.ActiveScene());
    window_.PollEvents();
    if (window_.ShouldClose())
        return;

    // A queued animation capture owns the frame (its own UI frame and render
    // loop) and skips the normal tick.
    if (capture_.RunPending())
        return;

    // The export dialog's live preview poses the model from the schedule the
    // recording uses. Here rather than inside the UI frame, because it mutates
    // the actor, which the frame build must not.
    ui_->Export().Tick(dt);

    // The async provider's completion queue, before any per-frame asset access,
    // so callbacks land before the rest of the tick reads what they produced.
    if (auto* cp = service_.Scene().ActiveContentProvider())
        cp->Pump();

    // Task completions, on the same thread and for the same reason. After the
    // provider's pump: a task body blocked in ReadFile is woken by that, and
    // delivering its completion first would report a task that has not returned.
    tasks_.Pump();

    // Before the UI frame, so the modal opens in the frame the task is submitted.
    loader_.OpenNextQueued(tasks_.Busy());

    // The rest of the frame waits out a minimised window.
    if (!window_.SyncFramebufferSize())
        return;

    // A document on its own scene needs its clock ticked here; inactive tabs keep
    // theirs, so switching back resumes where they left off.
    if (documents_.Active())
        service_.Scene().Update(dt);

    playback_.AdvanceWalkDrift(dt);

    if (auto* dnc = service_.GetDncService())
        dnc->Advance(dt);

    // Pump the panel's own provider, apply staged navigation and mark its cells
    // not yet visible — before the panel's BuildWindow acquires visible cells.
    explorer_.NewFrame(dt);

    window_.BeginImGuiFrame();
    ui_->BuildFrame();
    ImGui::Render();

    UpdateEngine();

    playback_.AdvanceEffectReframe();

    // Every visible thumbnail into its offscreen target BEFORE the main pass
    // composites the draw data that samples them. It juggles the active scene
    // per cell, so the document's is re-published after.
    if (explorer_.RenderThumbnails(dt))
        service_.SetActiveScene(documents_.ActiveScene());

    RenderViewport();
    UpdateTitle(dt);
}

void ViewerApp::UpdateEngine() {
    const f32 parentDt = playback_.ParentClockStep();
    playback_.UpdateCameraPresetAnimator();
    (void)service_.Replaceables().ConsumeDirty();
    (void)service_.Settings().ConsumeRenderModeDirty();

    // The camera pose to the sound emitter before the tick fires SND events, so
    // 3D-positioned event objects pan and attenuate against this frame's camera.
    const auto& cam = service_.Scene().Camera();
    const Vector3f eye = cam.GetSource();
    service_.Sound().SetListener(eye, cam.GetTarget() - eye, cam.GetUp());

    // Tick publishes the scene and restores the default one on exit, so the
    // document's is re-published for the effect framing and the render.
    service_.Ticker().Tick(service_.SceneAt(documents_.ActiveScene()), parentDt);
    service_.SetActiveScene(documents_.ActiveScene());
}

void ViewerApp::RenderViewport() {
    // RenderViewport names the scene explicitly, so a document on a non-default
    // scene composites correctly.
    Viewport vp;
    vp.scene = sceneHiddenForCapture_ ? service_.DefaultSceneId() : documents_.ActiveScene();
    vp.target = window_.Target();
    vp.camera = &service_.SceneAt(vp.scene).Camera();
    service_.Pipeline().RenderViewport(vp);
    service_.Pipeline().Present(window_.Target());
}

void ViewerApp::UpdateTitle(f32 dt) {
    fpsAccum_ += static_cast<f64>(dt);
    fpsFrames_ += 1;
    if (fpsAccum_ < 1.0)
        return;
    i32 nGeo = 0, nTex = 0, nNodes = 0, nParts = 0, nSegs = 0;
    service_.Pipeline().GetFrameStats(nGeo, nTex, nNodes, nParts, nSegs);
    char title[300];
    std::snprintf(title, sizeof(title), "%s — %d FPS | %d geo, %d tex, %d nodes, %d parts, %d segs",
                  ui::kWindowTitle, fpsFrames_, nGeo, nTex, nNodes, nParts, nSegs);
    window_.SetTitle(title);
    fpsAccum_ = 0.0;
    fpsFrames_ = 0;
}

} // namespace whiteout::flakes
