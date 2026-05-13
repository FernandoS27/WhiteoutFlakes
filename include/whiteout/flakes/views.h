#pragma once

// ============================================================================
// WhiteoutFlakes — sub-service view classes returned by Renderer.
//
// Each view is a lightweight handle pointing at the renderer's internal
// state. They expose a curated set of operations on a single subsystem
// (pipeline, camera, settings, etc.) and forward into the existing internal
// classes (RenderPipeline, Camera, RenderSettings, ...).
//
// Views don't own the impl; they're invalid after the Renderer is destroyed.
// They are created exclusively by Renderer accessors.
// ============================================================================

#include "content_provider.h"
#include "display.h"
#include "enums.h"
#include "model_data.h"
#include "model_source.h"
#include "types.h"
#include "util/replaceable_paths.h" // Tileset

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace whiteout::flakes {

// Opaque handle for a renderer-owned actor. 0 = invalid.
using ActorHandle = u32;

namespace detail {
class RendererImpl;
}

class PipelineView {
public:
    void InitDevice(GfxApi);
    bool IsDeviceReady() const;
    RenderTargetId CreateSwapChainTarget(void* hwnd, i32 width, i32 height);
    void SetPrimaryTarget(RenderTargetId);
    void ResizePrimaryTarget(i32 width, i32 height);
    void RenderFrame(RenderTargetId);
    void Present(RenderTargetId);
    void Shutdown();
    FrameStats GetFrameStats() const;

private:
    explicit PipelineView(detail::RendererImpl* impl) : impl_(impl) {}
    detail::RendererImpl* impl_;
    friend class Renderer;
};

class SceneView {
public:
    i32 AnimationTimeMs() const;
    void SetAnimationTimeMs(i32);
    void Update(f32 dt);
    void SetPE1BasePath(const std::filesystem::path&);
    IContentProvider* ActiveContentProvider();

private:
    explicit SceneView(detail::RendererImpl* impl) : impl_(impl) {}
    detail::RendererImpl* impl_;
    friend class Renderer;
};

class CameraView {
public:
    enum class Mode { Orbital, Direct };

    void Reset();
    void SetPitch(f32);
    void SetYaw(f32);
    void SetDistance(f32);
    void SetTarget(f32 x, f32 y, f32 z);
    void Rotate(i32 dx, i32 dy);
    void Pan(i32 dx, i32 dy);
    void Zoom(i32 wheelDelta);
    void ZoomSmooth(f32 factor);
    void SetOrbitalMode();
    void SetFovDiagonal(f32);
    void SetClip(f32 nz, f32 fz);
    void SetDirectPose(Vector3f pos, Vector3f target, f32 roll);
    Mode GetMode() const;
    Vector3f GetTarget() const;
    f32 GetDistance() const;

    static const f32 kFactorRelDist;
    static const f32 kDefaultFovDiagonal;
    static const f32 kDefaultNearZ;
    static const f32 kDefaultFarZ;

private:
    explicit CameraView(detail::RendererImpl* impl) : impl_(impl) {}
    detail::RendererImpl* impl_;
    friend class Renderer;
};

class SettingsView {
public:
    DisplayFlags GetDisplayFlags() const;
    void SetDisplayFlags(const DisplayFlags&);
    bool ConsumeRenderModeDirty();

    LightingMode GetLightingMode() const;
    void SetLightingMode(LightingMode);

    u32 BackgroundColorRaw() const;
    void SetBackgroundColor(u8 r, u8 g, u8 b);

    f32 GetTonemapExposure() const;
    void SetTonemapExposure(f32);

    IblMode GetIblMode() const;
    void SetIblMode(IblMode);

    i32 HdDebugMode() const;
    void SetHdDebugMode(i32);

    i32 LodOverride() const;
    void SetLodOverride(i32);

    void SetRenderMode(RenderMode);

private:
    explicit SettingsView(detail::RendererImpl* impl) : impl_(impl) {}
    detail::RendererImpl* impl_;
    friend class Renderer;
};

class LoaderView {
public:
    ActorHandle SpawnUnit(const std::string& path);
    ActorHandle SpawnUnitFromSource(std::shared_ptr<IModelSource> source,
                                    const Matrix44f& initialTransform = Matrix44f::identity());
    void UpdateMaterials(ActorHandle handle, const std::vector<MaterialData>& materials,
                         const std::vector<TextureData>& textures);
    void RequestClearAll();

private:
    explicit LoaderView(detail::RendererImpl* impl) : impl_(impl) {}
    detail::RendererImpl* impl_;
    friend class Renderer;
};

class DebugView {
public:
    i32 HitTestViewCube(i32 mouseX, i32 mouseY);
    Rect GetViewCubeRect() const;
    void SetViewCubeHovered(bool);

private:
    explicit DebugView(detail::RendererImpl* impl) : impl_(impl) {}
    detail::RendererImpl* impl_;
    friend class Renderer;
};

class DncView {
public:
    bool IsValid() const;
    f32 GetTimeOfDay() const;
    void SetTimeOfDay(f32);
    f32 GetTodScale() const;
    void SetTodScale(f32);
    f32 GetHoursPerDay() const;
    const std::string& UnitMdlPath() const;
    void SetUnitMdl(const std::string&);
    void Advance(f32);

private:
    explicit DncView(detail::RendererImpl* impl) : impl_(impl) {}
    detail::RendererImpl* impl_;
    friend class Renderer;
};

class ShadowView {
public:
    bool IsValid() const;
    bool IsEnabled() const;
    ShadowParams Params() const;
    void SetParams(const ShadowParams&);

private:
    explicit ShadowView(detail::RendererImpl* impl) : impl_(impl) {}
    detail::RendererImpl* impl_;
    friend class Renderer;
};

class SplatView {
public:
    void Clear();

private:
    explicit SplatView(detail::RendererImpl* impl) : impl_(impl) {}
    detail::RendererImpl* impl_;
    friend class Renderer;
};

class ReplaceablesView {
public:
    bool ConsumeDirty();
    void SetTileset(Tileset);

private:
    explicit ReplaceablesView(detail::RendererImpl* impl) : impl_(impl) {}
    detail::RendererImpl* impl_;
    friend class Renderer;
};

} // namespace whiteout::flakes
