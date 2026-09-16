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
#include "whiteout/flakes/event_data.h"
#include "whiteout/flakes/renderer.h"
#include "whiteout/flakes/views.h"

#include "io/mdx_model_adapter.h"
#include "renderer/assets/asset_manager.h"
#include "renderer/assets/replaceable_texture_manager.h"
#include "renderer/camera.h"
#include "renderer/debug/debug_renderer.h"
#include "renderer/dnc/dnc_service.h"
#include "renderer/frame_ticker.h"
#include "renderer/model/model_instance.h"
#include "renderer/model/corn_effect_source.h"
#include "renderer/model/model_loader.h"
#include "renderer/corn_effects/corn_effects_service.h"
#include "renderer/effects/spn_spawner.h"
#include "renderer/model/model_template.h"
#include "renderer/particle/particle_service.h"
#include "renderer/effects/splat_service.h"
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
CameraHandle Renderer::CreateCamera() {
    return static_cast<CameraHandle>(impl_->scene_.AddCamera());
}
CameraView Renderer::CameraAt(CameraHandle h) {
    return CameraView(impl_.get(), h);
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
AssetsView Renderer::Assets() {
    return AssetsView(impl_.get());
}
PlaybackView Renderer::Playback() {
    return PlaybackView(impl_.get());
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
RenderTargetId PipelineView::CreateOffscreenTarget(i32 w, i32 h) {
    return Svc(impl_).Pipeline().CreateOffscreenTarget(w, h);
}
void PipelineView::DestroyTarget(RenderTargetId id) {
    Svc(impl_).Pipeline().DestroyTarget(id);
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
void PipelineView::RenderViewport(RenderTargetId id, CameraHandle camera) {
    ::whiteout::flakes::renderer::Viewport vp;
    vp.target = id;
    vp.camera = &Scn(impl_).Camera(static_cast<i32>(camera));
    Svc(impl_).Pipeline().RenderViewport(vp);
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
    // Separate call because corn effects are a separate simulation: the
    // pipeline's own stats cover the legacy particle service only.
    s.cornParticles = Svc(impl_).CornEffects().TotalParticleCount();
    return s;
}

u64 PipelineView::LiveGpuBytes() const {
    auto* gfx = Svc(impl_).Pipeline().Gfx();
    return gfx ? gfx->LiveGpuBytes() : 0;
}

std::vector<u8> PipelineView::ReadbackTarget(RenderTargetId t, i32& width, i32& height) {
    std::vector<u8> rgba;
    width = 0;
    height = 0;
    if (!Svc(impl_).Pipeline().ReadbackTarget(t, rgba, width, height))
        rgba.clear();
    return rgba;
}

// ============================================================================
// SceneView
// ============================================================================

// Public/internal ProductId are the same shape for the same reason
// AssetsView::Kind is: the public header cannot include renderer internals.
// One assert per value, so a renumbering is a build error rather than a scene
// silently picking the wrong render profile.
#define WDX_ASSERT_PRODUCT_ID(name)                                                                 \
    static_assert(static_cast<u8>(ProductId::name) ==                                              \
                      static_cast<u8>(renderer::core::ProductId::name),                            \
                  "ProductId::" #name " has drifted from renderer::core::ProductId::" #name)
WDX_ASSERT_PRODUCT_ID(Neutral);
WDX_ASSERT_PRODUCT_ID(Wc3);
WDX_ASSERT_PRODUCT_ID(Wow);
WDX_ASSERT_PRODUCT_ID(Sc2);
WDX_ASSERT_PRODUCT_ID(D3);
#undef WDX_ASSERT_PRODUCT_ID

ProductId SceneView::GetProduct() const {
    return Scn(impl_).Product();
}
void SceneView::SetProduct(ProductId p) {
    if (Scn(impl_).Product() == p)
        return;
    Scn(impl_).SetProduct(p);
    // The product selects the profile, so this is a frame change — same
    // re-stage trigger a render-mode flip fires. See
    // RenderSettings::MarkRenderModeDirty for why it keeps that name.
    Svc(impl_).Settings().MarkRenderModeDirty();
}

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
void SceneView::SetEngineAssetRoot(const std::filesystem::path& root) {
    // Deliberately the internal provider rather than ActiveContentProvider():
    // an external provider owns its own resolution and has no such root.
    // GetContentProvider() realises the internal one, matching SetPE1BasePath.
    Scn(impl_).GetContentProvider().SetSystemBasePath(root);
}
void SceneView::SetCascInstallPath(const std::string& root) {
    // The internal provider deliberately, matching SetEngineAssetRoot: an
    // external provider owns its own resolution and has no CASC of its own.
    Scn(impl_).GetContentProvider().SetInstallPath(root);
}
void SceneView::SetMpqList(const std::vector<std::string>& archives) {
    Scn(impl_).GetContentProvider().SetMpqList(archives);
}
void SceneView::SetHdMode(bool enabled) {
    Scn(impl_).GetContentProvider().SetHdMode(enabled);
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
inline Camera& Cam(detail::RendererImpl* p, CameraHandle h) {
    return Scn(p).Camera(static_cast<i32>(h));
}
} // namespace

void CameraView::Reset() {
    Cam(impl_, cam_).Reset();
}
void CameraView::SetPitch(f32 p) {
    Cam(impl_, cam_).SetPitch(p);
}
void CameraView::SetYaw(f32 y) {
    Cam(impl_, cam_).SetYaw(y);
}
void CameraView::SetDistance(f32 d) {
    Cam(impl_, cam_).SetDistance(d);
}
void CameraView::SetTarget(f32 x, f32 y, f32 z) {
    Cam(impl_, cam_).SetTarget(x, y, z);
}
void CameraView::Rotate(i32 dx, i32 dy) {
    Cam(impl_, cam_).Rotate(dx, dy);
}
void CameraView::Pan(i32 dx, i32 dy) {
    Cam(impl_, cam_).Pan(dx, dy);
}
void CameraView::Zoom(i32 wheel) {
    Cam(impl_, cam_).Zoom(wheel);
}
void CameraView::ZoomSmooth(f32 amount) {
    Cam(impl_, cam_).ZoomSmooth(amount);
}
void CameraView::SetOrbitalMode() {
    Cam(impl_, cam_).SetOrbitalMode();
}
void CameraView::SetFovDiagonal(f32 r) {
    Cam(impl_, cam_).SetFovDiagonal(r);
}
void CameraView::SetClip(f32 nz, f32 fz) {
    Cam(impl_, cam_).SetClip(nz, fz);
}
void CameraView::SetDirectPose(Vector3f p, Vector3f t, f32 roll) {
    Cam(impl_, cam_).SetDirectPose(p, t, roll);
}

CameraView::Mode CameraView::GetMode() const {
    return Cam(impl_, cam_).GetMode() == ::whiteout::flakes::renderer::Camera::Mode::Orbital
               ? Mode::Orbital
               : Mode::Direct;
}
Vector3f CameraView::GetTarget() const {
    return Cam(impl_, cam_).GetTarget();
}
f32 CameraView::GetDistance() const {
    return Cam(impl_, cam_).GetDistance();
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
bool SettingsView::GetBloomEnabled() const {
    return Svc(impl_).Settings().BloomEnabled();
}
void SettingsView::SetBloomEnabled(bool on) {
    Svc(impl_).Settings().SetBloomEnabled(on);
}
IblMode SettingsView::GetIblMode() const {
    return Svc(impl_).Settings().GetIblMode();
}
void SettingsView::SetIblMode(IblMode m) {
    Svc(impl_).Settings().SetIblMode(m);
}
DebugView SettingsView::GetDebugView() const {
    return Svc(impl_).Settings().GetDebugView();
}
void SettingsView::SetDebugView(DebugView v) {
    Svc(impl_).Settings().SetDebugView(v);
}
i32 SettingsView::HdDebugMode() const {
    return Svc(impl_).Settings().HdDebugMode();
}
void SettingsView::SetHdDebugMode(i32 m) {
    Svc(impl_).Settings().SetHdDebugMode(m);
}
u32 SettingsView::AoQuality() const {
    return Svc(impl_).Settings().AoQuality();
}
void SettingsView::SetAoQuality(u32 q) {
    Svc(impl_).Settings().SetAoQuality(q);
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

ActorHandle LoaderView::SpawnUnitFromRef(const ContentRef& ref) {
    auto* a = Svc(impl_).Loader().SpawnUnit(ref);
    return a ? a->handle : 0;
}

ActorHandle LoaderView::SpawnEffect(const std::string& path) {
    auto* a = Svc(impl_).Loader().SpawnUnitFromSource(
        std::make_shared<renderer::model::CornEffectSource>(path));
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
void LoaderView::Destroy(ActorHandle handle) {
    Svc(impl_).Loader().DestroyActor(handle);
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
void ShadowView::SetEnabled(bool on) {
    if (auto* s = Svc(impl_).GetShadowService())
        s->SetEnabled(on);
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
// AssetsView — surface for AssetManager (Phase-1 skeleton).
// ============================================================================

namespace {
// Public/internal AssetKind enums are intentionally kept in sync — one
// belongs to the public header (no internal includes), one to the
// renderer impl. Convert at the view boundary.
//
// The conversion is a numeric cast, so a renumbering on either side
// compiles cleanly and mis-routes at runtime — a Model arriving as an
// Effect, parsed by the wrong decoder. `AssetsView::Kind` is also `@bind`,
// which puts the same wrong number in the C ABI and the Rust crate. One
// assert per value makes the next divergence a build error instead.
#define WDX_ASSERT_ASSET_KIND(name)                                                                \
    static_assert(static_cast<u8>(AssetsView::Kind::name) ==                                       \
                      static_cast<u8>(renderer::assets::AssetKind::name),                          \
                  "AssetsView::Kind::" #name " has drifted from "                                  \
                  "renderer::assets::AssetKind::" #name)
WDX_ASSERT_ASSET_KIND(Texture);
WDX_ASSERT_ASSET_KIND(Model);
WDX_ASSERT_ASSET_KIND(Effect);
WDX_ASSERT_ASSET_KIND(Data);
#undef WDX_ASSERT_ASSET_KIND

inline renderer::assets::AssetKind ToInternal(AssetsView::Kind k) {
    return static_cast<renderer::assets::AssetKind>(static_cast<u8>(k));
}
inline AssetsView::Kind ToPublic(renderer::assets::AssetKind k) {
    return static_cast<AssetsView::Kind>(static_cast<u8>(k));
}
} // namespace

void AssetsView::DrainNeeds(const AssetsView::NeededFn& cb) {
    if (!impl_) return;
    Svc(impl_).Assets().DrainNeeds(
        [&cb](renderer::assets::AssetKind k, renderer::assets::AssetSubKind sub,
              const ContentRef& ref) {
            if (cb) cb(ToPublic(k), sub, ref);
        });
}

bool AssetsView::ApplyAsset(AssetsView::Kind kind, u8 subKind, const ContentRef& ref,
                            std::span<const u8> bytes, std::string_view foundExt) {
    if (!impl_) return false;
    return Svc(impl_).Assets().ApplyPrepared(ToInternal(kind), subKind, ref, bytes, foundExt);
}

AssetsView::Stats AssetsView::GetStats() const {
    AssetsView::Stats out;
    if (!impl_) return out;
    auto s = Svc(impl_).Assets().GetStats();
    out.liveSlots        = s.liveSlots;
    out.loadedSlots      = s.loadedSlots;
    out.pendingNeeds     = Svc(impl_).Assets().PendingNeedsCount();
    out.totalAcquires    = s.totalAcquires;
    out.totalReleases    = s.totalReleases;
    out.totalApplies     = s.totalApplies;
    out.totalApplyMisses = s.totalApplyMisses;
    return out;
}

std::size_t AssetsView::RetryUnloaded() {
    if (!impl_) return 0;
    return Svc(impl_).Assets().RetryUnloaded();
}

void AssetsView::PrefetchEventAssets() {
    if (!impl_) return;
    io::PrefetchEventAssetSlots(Svc(impl_).Assets());
}

void AssetsView::PrefetchEventAssetsForActor(ActorHandle handle) {
    if (!impl_) return;
    auto* a = FindActor(impl_, handle);
    if (!a || !a->sourceTemplate) return;
    io::PrefetchEventAssetSlotsForEvents(
        Svc(impl_).Assets(), a->sourceTemplate->eventObjects);
}

bool AssetsView::IsTextureCached(std::string_view path) const {
    if (!impl_) return false;
    return Svc(impl_).Assets().IsTextureCached(path);
}

namespace detail {
// Public AssetPreload is a pimpl over the renderer-internal bundle, which
// is what actually owns the slot references.
class AssetPreloadState {
public:
    explicit AssetPreloadState(renderer::assets::AssetPreload preload)
        : preload_(std::move(preload)) {}
    renderer::assets::AssetPreload preload_;
};
} // namespace detail

AssetPreload::AssetPreload() = default;
AssetPreload::AssetPreload(std::unique_ptr<detail::AssetPreloadState> state)
    : state_(std::move(state)) {}
AssetPreload::~AssetPreload() = default;
AssetPreload::AssetPreload(AssetPreload&&) noexcept = default;
AssetPreload& AssetPreload::operator=(AssetPreload&&) noexcept = default;

std::size_t AssetPreload::Count() const {
    return state_ ? state_->preload_.Count() : 0;
}
std::size_t AssetPreload::LoadedCount() const {
    return state_ ? state_->preload_.LoadedCount() : 0;
}
bool AssetPreload::Ready() const {
    return !state_ || state_->preload_.Ready();
}
std::vector<std::string> AssetPreload::Paths() const {
    if (!state_) return {};
    const auto& refs = state_->preload_.Refs();
    std::vector<std::string> out;
    out.reserve(refs.size());
    for (const auto& r : refs)
        out.push_back(r.Describe());
    return out;
}
void AssetPreload::Release() {
    state_.reset();
}

AssetPreload AssetsView::Preload(AssetsView::Kind kind, std::span<const std::string> paths) {
    if (!impl_) return {};
    // Path-typed on the public surface: every caller of this is a host
    // enumerating files it can name. Id-addressed preloads have no host-side
    // use case yet — the format that needs them resolves siblings internally.
    std::vector<ContentRef> refs;
    refs.reserve(paths.size());
    for (const auto& p : paths)
        refs.push_back(ContentRef::FromPath(p));
    return AssetPreload(std::make_unique<detail::AssetPreloadState>(
        Svc(impl_).PreloadAssets(ToInternal(kind), renderer::assets::kSoleSubKind, refs)));
}

AssetPreload AssetsView::PreloadDirectory(AssetsView::Kind kind, std::string_view directory,
                                          bool recursive) {
    if (!impl_) return {};
    return AssetPreload(std::make_unique<detail::AssetPreloadState>(Svc(impl_).PreloadAssetDirectory(
        ToInternal(kind), renderer::assets::kSoleSubKind, directory, recursive)));
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

DebugViewFamily ActorView::DebugFamily() const {
    return Svc(impl_).DebugFamilyOf(handle_);
}

bool ActorView::EffectBounds(i32 emitterId, Vector3f& outMin, Vector3f& outMax) const {
    // Reads the ACTIVE scene's corn service, so a multi-scene host must
    // publish the effect's scene before asking.
    return Svc(impl_).ComputeEffectWorldBounds(handle_, emitterId, outMin, outMax);
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
    auto* a = FindActor(impl_, handle_);
    if (!a)
        return;
    a->animation.SetTimeMs(t);

    // Setting the time alone does not hold for a renderer-driven actor:
    // `Actor::Advance` recomputes the frame from `cursor.actorTimeMs` on
    // every tick, so the next one would overwrite it and a host scrubbing a
    // timeline would see nothing move. Re-base the play's start so the
    // playlist already reads as `t`, and Advance recomputes the same value.
    //
    // External actors are evaluated by the host and never see Advance, so
    // the time set above is already the whole story for them.
    if (a->role == renderer::model::ActorRole::External || !a->animation.HasSource())
        return;
    const auto seqs = a->animation.Sequences();
    if (seqs.empty())
        return;
    a->animation.Playlist().SetPrimaryTimeMs(t, a->cursor.actorTimeMs, seqs);
}
bool ActorView::HasAnimationSource() const {
    auto* a = FindActor(impl_, handle_);
    return a && a->animation.HasSource();
}

u32 ActorView::Play(i32 sequence, f32 weight, f32 speed, bool loop, i32 blendInMs,
                    i32 blendOutMs) {
    auto* a = FindActor(impl_, handle_);
    if (!a || !a->animation.HasSource())
        return 0;
    renderer::animation::PlayDesc d;
    d.sequence = sequence;
    d.weight = weight;
    d.speed = speed;
    d.loop = loop;
    d.blendInMs = blendInMs;
    d.blendOutMs = blendOutMs;
    // Host-started plays are exempt from the covered-play cull. The cull exists
    // to drop plays a later full-weight one has buried, which is right for the
    // ones a sequence switch spawns; a host that asked for this play by hand is
    // holding a handle to it and expects to be the one to stop it.
    d.persistent = true;
    return a->animation.Playlist().Play(d, a->cursor.actorTimeMs);
}

void ActorView::StopPlay(u32 playHandle, i32 blendOutMs) {
    if (auto* a = FindActor(impl_, handle_))
        a->animation.Playlist().Stop(playHandle, blendOutMs, a->cursor.actorTimeMs);
}

void ActorView::StopAllPlays(i32 blendOutMs) {
    if (auto* a = FindActor(impl_, handle_))
        a->animation.Playlist().StopAll(blendOutMs, a->cursor.actorTimeMs);
}

i32 ActorView::PlayCount() const {
    auto* a = FindActor(impl_, handle_);
    return a ? static_cast<i32>(a->animation.Playlist().PlayCount()) : 0;
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
    return (a && a->render.surfaceTable) ? static_cast<i32>(a->render.surfaceTable->Count()) : 0;
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

RenderMode ActorView::PreferredRenderMode() const {
    auto* a = FindActor(impl_, handle_);
    return a ? a->PreferredRenderMode() : RenderMode::SD;
}

std::vector<std::string> ActorView::ChildModelPaths() const {
    std::vector<std::string> out;
    auto* a = FindActor(impl_, handle_);
    if (!a || !a->sourceTemplate)
        return out;
    const auto& t = *a->sourceTemplate;
    out.reserve(t.attachmentConfigs.size() + t.pe1Configs.size());
    for (const auto& ac : t.attachmentConfigs)
        if (!ac.modelPath.empty()) out.push_back(ac.modelPath);
    for (const auto& pc : t.pe1Configs)
        if (!pc.modelPath.empty()) out.push_back(pc.modelPath);
    return out;
}

// ============================================================================
// PlaybackView
// ============================================================================

PlaybackState PlaybackView::GetState() const {
    return Scn(impl_).GetPlaybackState();
}
void PlaybackView::SetState(PlaybackState s) {
    if (s == PlaybackState::Stopped)
        Svc(impl_).RewindScene();
    Scn(impl_).SetPlaybackState(s);
}

void PlaybackView::Play() {
    Scn(impl_).SetPlaybackState(PlaybackState::Playing);
}
void PlaybackView::Pause() {
    Scn(impl_).SetPlaybackState(PlaybackState::Paused);
}
void PlaybackView::Stop() {
    SetState(PlaybackState::Stopped);
}
void PlaybackView::Restart() {
    Svc(impl_).RewindScene();
    Scn(impl_).SetPlaybackState(PlaybackState::Playing);
}

void PlaybackView::ResyncEffects() {
    Svc(impl_).DropTransientEffects();
}

bool PlaybackView::IsPaused() const {
    return Scn(impl_).IsPaused();
}

f32 PlaybackView::GetTimeScale() const {
    return Scn(impl_).GetTimeScale();
}
void PlaybackView::SetTimeScale(f32 s) {
    Scn(impl_).SetTimeScale(s);
}

} // namespace whiteout::flakes
