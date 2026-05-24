// ============================================================================
// WhiteoutFlakes — public Renderer / View / ActorView implementation.
//
// The public include/whiteout/flakes/*.h headers expose a thin, opaque
// facade. This file is the body. RendererImpl owns the existing internal
// classes (SceneManager, RenderService) and every public method forwards
// into them. View classes carry an opaque RendererImpl* and dispatch via
// the helpers at the bottom of this file.
// ============================================================================

#include "whiteout/flakes/actor_view.h"
#include "whiteout/flakes/renderer.h"
#include "whiteout/flakes/views.h"

#include "renderer/assets/replaceable_texture_manager.h"
#include "renderer/camera.h"
#include "renderer/debug/debug_renderer.h"
#include "renderer/dnc/dnc_service.h"
#include "renderer/frame_ticker.h"
#include "renderer/model/model_instance.h"
#include "renderer/model/model_loader.h"
#include "renderer/model/model_template.h"
#include "renderer/particle/splat_service.h"
#include "renderer/render_pipeline.h"
#include "renderer/render_service.h"
#include "renderer/render_settings.h"
#include "renderer/scene_manager.h"
#include "renderer/shadow/shadow_service.h"

namespace whiteout::flakes {

namespace {
using ::whiteout::flakes::renderer::Camera;
using ::whiteout::flakes::renderer::RenderService;
using ::whiteout::flakes::renderer::SceneManager;
using ::whiteout::flakes::renderer::model::Actor;
} // namespace

namespace detail {

class RendererImpl {
public:
    RendererImpl() : scene_(), service_(scene_) {}

    SceneManager scene_;
    RenderService service_;
};

} // namespace detail

// ============================================================================
// Renderer
// ============================================================================

Renderer::Renderer() : impl_(std::make_unique<detail::RendererImpl>()) {}
Renderer::~Renderer() = default;

PipelineView Renderer::Pipeline() {
    return PipelineView(impl_.get());
}
SceneView Renderer::Scene() {
    return SceneView(impl_.get());
}
CameraView Renderer::Camera() {
    return CameraView(impl_.get());
}
SettingsView Renderer::Settings() {
    return SettingsView(impl_.get());
}
LoaderView Renderer::Loader() {
    return LoaderView(impl_.get());
}
DncView Renderer::Dnc() {
    return DncView(impl_.get());
}
ShadowView Renderer::Shadow() {
    return ShadowView(impl_.get());
}
SplatView Renderer::Splats() {
    return SplatView(impl_.get());
}
ReplaceablesView Renderer::Replaceables() {
    return ReplaceablesView(impl_.get());
}

ActorView Renderer::Actor(ActorHandle h) {
    return ActorView(impl_.get(), h);
}

void Renderer::SwapSoundEmitter(std::unique_ptr<ISoundEmitter> e) {
    impl_->service_.SwapSoundEmitter(std::move(e));
}

void Renderer::Tick(f32 dt) {
    impl_->service_.Ticker().Tick(dt);
}

bool Renderer::IsTextureCached(std::string_view sharedKey) const {
    return impl_->service_.HasCachedTexture(sharedKey);
}

// ============================================================================
// Internal helpers — every view forwards through one of these.
// ============================================================================
namespace {

inline RenderService& Svc(detail::RendererImpl* p) {
    return p->service_;
}
inline SceneManager& Scn(detail::RendererImpl* p) {
    return p->scene_;
}

inline Actor* FindActor(detail::RendererImpl* p, ActorHandle h) {
    if (h == 0)
        return nullptr;
    return Scn(p).Actors().Find(h);
}

} // namespace

// ============================================================================
// PipelineView
// ============================================================================

void PipelineView::InitDevice(GfxApi api) {
    Svc(impl_).Pipeline().InitDevice(api);
}
bool PipelineView::IsDeviceReady() const {
    return Svc(impl_).Pipeline().IsDeviceReady();
}

RenderTargetId PipelineView::CreateSwapChainTarget(void* hwnd, i32 w, i32 h) {
    return Svc(impl_).Pipeline().CreateSwapChainTarget(hwnd, w, h);
}
void PipelineView::SetPrimaryTarget(RenderTargetId id) {
    Svc(impl_).Pipeline().SetPrimaryTarget(id);
}
void PipelineView::ResizePrimaryTarget(i32 w, i32 h) {
    Svc(impl_).Pipeline().ResizePrimaryTarget(w, h);
}
void PipelineView::RenderFrame(RenderTargetId id) {
    Svc(impl_).Pipeline().RenderFrame(id);
}
void PipelineView::Present(RenderTargetId id) {
    Svc(impl_).Pipeline().Present(id);
}
void PipelineView::Shutdown() {
    Svc(impl_).Pipeline().Shutdown();
}

FrameStats PipelineView::GetFrameStats() const {
    FrameStats s{};
    Svc(impl_).Pipeline().GetFrameStats(s.geosets, s.textures, s.nodes, s.particles, s.segments);
    return s;
}

// ============================================================================
// SceneView
// ============================================================================

i32 SceneView::AnimationTimeMs() const {
    return Scn(impl_).GetAnimationTime();
}
void SceneView::SetAnimationTimeMs(i32 ms) {
    Scn(impl_).SetAnimationTime(ms);
}
void SceneView::Update(f32 dt) {
    Scn(impl_).Update(dt);
}
void SceneView::SetPE1BasePath(const std::filesystem::path& p) {
    Scn(impl_).SetPE1BasePath(p);
}
IContentProvider* SceneView::ActiveContentProvider() {
    return Scn(impl_).ActiveContentProvider();
}
void SceneView::SetContentProvider(std::shared_ptr<IContentProvider> provider) {
    Scn(impl_).SetContentProvider(std::move(provider));
}

// ============================================================================
// CameraView
// ============================================================================

const f32 CameraView::kFactorRelDist = ::whiteout::flakes::renderer::Camera::kFactorRelDist;
const f32 CameraView::kDefaultFovDiagonal =
    ::whiteout::flakes::renderer::Camera::kDefaultFovDiagonal;
const f32 CameraView::kDefaultNearZ = ::whiteout::flakes::renderer::Camera::kDefaultNearZ;
const f32 CameraView::kDefaultFarZ = ::whiteout::flakes::renderer::Camera::kDefaultFarZ;

namespace {
inline Camera& Cam(detail::RendererImpl* p) {
    return Scn(p).Camera();
}
} // namespace

void CameraView::Reset() {
    Cam(impl_).Reset();
}
void CameraView::SetPitch(f32 p) {
    Cam(impl_).SetPitch(p);
}
void CameraView::SetYaw(f32 y) {
    Cam(impl_).SetYaw(y);
}
void CameraView::SetDistance(f32 d) {
    Cam(impl_).SetDistance(d);
}
void CameraView::SetTarget(f32 x, f32 y, f32 z) {
    Cam(impl_).SetTarget(x, y, z);
}
void CameraView::Rotate(i32 dx, i32 dy) {
    Cam(impl_).Rotate(dx, dy);
}
void CameraView::Pan(i32 dx, i32 dy) {
    Cam(impl_).Pan(dx, dy);
}
void CameraView::Zoom(i32 wheel) {
    Cam(impl_).Zoom(wheel);
}
void CameraView::ZoomSmooth(f32 amount) {
    Cam(impl_).ZoomSmooth(amount);
}
void CameraView::SetOrbitalMode() {
    Cam(impl_).SetOrbitalMode();
}
void CameraView::SetFovDiagonal(f32 r) {
    Cam(impl_).SetFovDiagonal(r);
}
void CameraView::SetClip(f32 nz, f32 fz) {
    Cam(impl_).SetClip(nz, fz);
}
void CameraView::SetDirectPose(Vector3f p, Vector3f t, f32 roll) {
    Cam(impl_).SetDirectPose(p, t, roll);
}

CameraView::Mode CameraView::GetMode() const {
    return Cam(impl_).GetMode() == ::whiteout::flakes::renderer::Camera::Mode::Orbital
               ? Mode::Orbital
               : Mode::Direct;
}
Vector3f CameraView::GetTarget() const {
    return Cam(impl_).GetTarget();
}
f32 CameraView::GetDistance() const {
    return Cam(impl_).GetDistance();
}

// ============================================================================
// SettingsView
// ============================================================================

DisplayFlags SettingsView::GetDisplayFlags() const {
    return Svc(impl_).Settings().GetDisplayFlags();
}
void SettingsView::SetDisplayFlags(const DisplayFlags& f) {
    Svc(impl_).Settings().SetDisplayFlags(f);
}
bool SettingsView::ConsumeRenderModeDirty() {
    return Svc(impl_).Settings().ConsumeRenderModeDirty();
}
LightingMode SettingsView::GetLightingMode() const {
    return Svc(impl_).Settings().GetLightingMode();
}
void SettingsView::SetLightingMode(LightingMode m) {
    Svc(impl_).Settings().SetLightingMode(m);
}
u32 SettingsView::BackgroundColorRaw() const {
    return Svc(impl_).Settings().BackgroundColorRaw();
}
void SettingsView::SetBackgroundColor(u8 r, u8 g, u8 b) {
    Svc(impl_).Settings().SetBackgroundColor(r, g, b);
}
f32 SettingsView::GetTonemapExposure() const {
    return Svc(impl_).Settings().GetTonemapExposure();
}
void SettingsView::SetTonemapExposure(f32 e) {
    Svc(impl_).Settings().SetTonemapExposure(e);
}
IblMode SettingsView::GetIblMode() const {
    return Svc(impl_).Settings().GetIblMode();
}
void SettingsView::SetIblMode(IblMode m) {
    Svc(impl_).Settings().SetIblMode(m);
}
i32 SettingsView::HdDebugMode() const {
    return Svc(impl_).Settings().HdDebugMode();
}
void SettingsView::SetHdDebugMode(i32 m) {
    Svc(impl_).Settings().SetHdDebugMode(m);
}
i32 SettingsView::LodOverride() const {
    return Svc(impl_).Settings().LodOverride();
}
void SettingsView::SetLodOverride(i32 l) {
    Svc(impl_).Settings().SetLodOverride(l);
}
void SettingsView::SetRenderMode(RenderMode m) {
    Svc(impl_).Settings().SetRenderMode(m);
}

// ============================================================================
// LoaderView
// ============================================================================

ActorHandle LoaderView::SpawnUnit(const std::string& path) {
    auto* a = Svc(impl_).Loader().SpawnUnit(path);
    return a ? a->handle : 0;
}

ActorHandle LoaderView::SpawnUnitFromSource(std::shared_ptr<IModelSource> src,
                                            const Matrix44f& initial) {
    auto* a = Svc(impl_).Loader().SpawnUnitFromSource(std::move(src), initial);
    return a ? a->handle : 0;
}

void LoaderView::UpdateMaterials(ActorHandle handle, const std::vector<MaterialData>& mats,
                                 const std::vector<TextureData>& texs) {
    Svc(impl_).Loader().UpdateMaterials(handle, mats, texs);
}

void LoaderView::RequestClearAll() {
    Svc(impl_).Loader().RequestClearAll();
}
void LoaderView::ClearTemplateCache() {
    Scn(impl_).Templates().Clear();
}

// ============================================================================
// DncView
// ============================================================================

bool DncView::IsValid() const {
    return Svc(impl_).GetDncService() != nullptr;
}
f32 DncView::GetTimeOfDay() const {
    return IsValid() ? Svc(impl_).GetDncService()->GetTimeOfDay() : 0.0f;
}
void DncView::SetTimeOfDay(f32 t) {
    if (auto* d = Svc(impl_).GetDncService())
        d->SetTimeOfDay(t);
}
f32 DncView::GetTodScale() const {
    return IsValid() ? Svc(impl_).GetDncService()->GetTodScale() : 1.0f;
}
void DncView::SetTodScale(f32 s) {
    if (auto* d = Svc(impl_).GetDncService())
        d->SetTodScale(s);
}
f32 DncView::GetHoursPerDay() const {
    return IsValid() ? Svc(impl_).GetDncService()->GetHoursPerDay() : 24.0f;
}

const std::string& DncView::UnitMdlPath() const {
    static const std::string kEmpty;
    auto* d = Svc(impl_).GetDncService();
    return d ? d->UnitMdlPath() : kEmpty;
}
void DncView::SetUnitMdl(const std::string& s) {
    if (auto* d = Svc(impl_).GetDncService())
        d->SetUnitMdl(s);
}
void DncView::Advance(f32 dt) {
    if (auto* d = Svc(impl_).GetDncService())
        d->Advance(dt);
}

// ============================================================================
// ShadowView
// ============================================================================

bool ShadowView::IsValid() const {
    return Svc(impl_).GetShadowService() != nullptr;
}
bool ShadowView::IsEnabled() const {
    return IsValid() && Svc(impl_).GetShadowService()->IsEnabled();
}
ShadowParams ShadowView::Params() const {
    return IsValid() ? Svc(impl_).GetShadowService()->Params() : ShadowParams{};
}
void ShadowView::SetParams(const ShadowParams& p) {
    if (auto* s = Svc(impl_).GetShadowService())
        s->SetParams(p);
}

// ============================================================================
// SplatView
// ============================================================================

void SplatView::Clear() {
    Svc(impl_).Splats().Clear();
}

// ============================================================================
// ReplaceablesView
// ============================================================================

bool ReplaceablesView::ConsumeDirty() {
    return Svc(impl_).Replaceables().ConsumeDirty();
}
void ReplaceablesView::SetTileset(Tileset t) {
    Svc(impl_).Replaceables().SetTileset(t);
}

// ============================================================================
// ActorView
// ============================================================================

bool ActorView::IsValid() const {
    return FindActor(impl_, handle_) != nullptr;
}

ActorRole ActorView::Role() const {
    auto* a = FindActor(impl_, handle_);
    return a ? static_cast<ActorRole>(a->role) : ActorRole::Unit;
}

Matrix44f ActorView::Transform() const {
    auto* a = FindActor(impl_, handle_);
    return a ? a->worldTransform : Matrix44f::identity();
}
void ActorView::SetTransform(const Matrix44f& m) {
    if (auto* a = FindActor(impl_, handle_))
        a->worldTransform = m;
}

f32 ActorView::PlaybackSpeed() const {
    auto* a = FindActor(impl_, handle_);
    return a ? a->playbackSpeed : 1.0f;
}
void ActorView::SetPlaybackSpeed(f32 s) {
    if (auto* a = FindActor(impl_, handle_))
        a->playbackSpeed = s;
}

bool ActorView::IgnoreNonLooping() const {
    auto* a = FindActor(impl_, handle_);
    return a ? a->ignoreNonLooping : false;
}
void ActorView::SetIgnoreNonLooping(bool v) {
    if (auto* a = FindActor(impl_, handle_))
        a->ignoreNonLooping = v;
}

u32 ActorView::TeamColor() const {
    auto* a = FindActor(impl_, handle_);
    return a ? a->teamColor : 0u;
}
void ActorView::SetTeamColor(u8 r, u8 g, u8 b) {
    if (auto* a = FindActor(impl_, handle_))
        a->SetTeamColor(r, g, b);
}

void ActorView::SetRoleExternal() {
    if (auto* a = FindActor(impl_, handle_))
        a->role = ::whiteout::flakes::renderer::model::ActorRole::External;
}

std::vector<SequenceInfo> ActorView::Sequences() const {
    auto* a = FindActor(impl_, handle_);
    return a ? a->animation.Sequences() : std::vector<SequenceInfo>{};
}
i32 ActorView::ActiveSequenceIndex() const {
    auto* a = FindActor(impl_, handle_);
    return a ? a->animation.ActiveSequenceIndex() : -1;
}
void ActorView::SetActiveSequence(i32 i) {
    if (auto* a = FindActor(impl_, handle_))
        a->animation.SetActiveSequenceIndex(i);
}
i32 ActorView::AnimationTimeMs() const {
    auto* a = FindActor(impl_, handle_);
    return a ? a->animation.TimeMs() : 0;
}
void ActorView::SetAnimationTimeMs(i32 t) {
    if (auto* a = FindActor(impl_, handle_))
        a->animation.SetTimeMs(t);
}
bool ActorView::HasAnimationSource() const {
    auto* a = FindActor(impl_, handle_);
    return a && a->animation.HasSource();
}

void ActorView::EvaluateAndApply() {
    if (auto* a = FindActor(impl_, handle_))
        a->EvaluateAndApply(Svc(impl_).MakeActorEvalContext());
}
void ActorView::EvaluateAt(i32 timeMs) {
    if (auto* a = FindActor(impl_, handle_)) {
        a->animation.SetTimeMs(timeMs);
        Scn(impl_).SetAnimationTime(timeMs);
        a->EvaluateAndApply(Svc(impl_).MakeActorEvalContext());
    }
}

i32 ActorView::GeosetCount() const {
    auto* a = FindActor(impl_, handle_);
    return a ? static_cast<i32>(a->render.gpuGeosets.size()) : 0;
}
i32 ActorView::MaterialCount() const {
    auto* a = FindActor(impl_, handle_);
    return a ? static_cast<i32>(a->render.gpuMaterials.size()) : 0;
}
i32 ActorView::CollisionShapeCount() const {
    auto* a = FindActor(impl_, handle_);
    return a ? static_cast<i32>(a->render.collisionShapes.size()) : 0;
}

std::vector<CameraPreset> ActorView::CameraPresets() const {
    auto* a = FindActor(impl_, handle_);
    if (!a || !a->sourceTemplate)
        return {};
    return a->sourceTemplate->cameraPresets;
}

} // namespace whiteout::flakes
