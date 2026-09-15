#pragma once

#include "gfx/gfx.h"
#include "renderer/types.h"
#include "whiteout/flakes/model_types.h"
#include "whiteout/flakes/shadow_params.h"
#include "whiteout/flakes/types.h"

#include <array>
#include <span>
#include <vector>

namespace whiteout::flakes::renderer::shadow {

// WC3 3.0.0 keeps its cascades in one Texture2DArray of three sets of three
// cascades: cascade `c` of set `s` is slice `3c + s`, and its matrix is
// cascadeSets[3s + c] in the HD PS bank. The lit pass reads set 2 (set 1 only
// under SHADOW_CASCADE2, which nothing here selects), so that is the only set
// rendered — the portrait configuration (WC3_30_LIGHTING_DESIGN.md D7).
inline constexpr i32 kCascadeSets = 3;
inline constexpr i32 kMaxCascades = 3;
inline constexpr i32 kRenderedCascadeSet = 2;

inline constexpr i32 CascadeSlice(i32 cascade, i32 set = kRenderedCascadeSet) {
    return cascade * kCascadeSets + set;
}

// Point-light cube shadows (WorldShadow_AllocatePointShadows): up to four
// shadow-casting omni lights, 512^2 faces in one cube array, slot `s` face `f`
// at layer 6s + f. The HD PS reads slot records from cb1 rows 43+ and treats
// a light's shadow index as valid only below the slot count, so live slots are
// packed from 0 every frame.
inline constexpr i32 kPointShadowSlots = 4;
inline constexpr i32 kPointShadowFaceSize = 512;
// The engine floors ShadowCastingStart here (CGxuLight+32).
inline constexpr f32 kPointShadowMinNear = 5.0f;
// Seconds a slot takes to fade fully in or out.
inline constexpr f32 kPointShadowFadeSeconds = 0.5f;

struct PointShadow {
    Vector3f position = {0, 0, 0};
    f32 nearZ = kPointShadowMinNear;
    f32 farZ = kPointShadowMinNear;
    f32 strength = 0.0f;
    // D3D cube-face order (+X, -X, +Y, -Y, +Z, -Z); world -> clip, row vectors.
    std::array<Matrix44f, 6> faceViewProj{};
};

// World -> clip for one face of a 90-degree cube camera at `eye`.
Matrix44f CubeFaceViewProj(const Vector3f& eye, i32 face, f32 nearZ, f32 farZ);

class ShadowService {
public:
    explicit ShadowService(gfx::IGFXDevice* gfx);
    ~ShadowService();

    ShadowService(const ShadowService&) = delete;
    ShadowService& operator=(const ShadowService&) = delete;

    void SetParams(const ShadowParams& p);
    const ShadowParams& Params() const {
        return params_;
    }

    void SetEnabled(bool on);
    bool IsEnabled() const {
        return params_.enabled;
    }

    void Update(const Matrix44f& cameraViewLH, const Matrix44f& cameraProjLH, f32 cameraNearZ,
                f32 cameraFarZ, const Vector3f& lightDirWS, const Vector3f& sceneCenterWS,
                f32 sceneRadius);

    i32 cascadeCount() const {
        return params_.cascadeCount;
    }
    // The D32 array every cascade renders into (Invalid until the first Update).
    gfx::TextureHandle DepthArray() const {
        return depthArray_;
    }
    const Matrix44f& cascadeVP(i32 c) const;

    // Cascades the shadow pass actually filled this frame, counted from
    // cascade 0. Update resets it; the lit pass samples only when it is > 0.
    void SetRenderedCascades(i32 n) {
        renderedCascades_ = n;
    }
    i32 RenderedCascades() const {
        return renderedCascades_;
    }

    // Reassign the point-shadow slots from this frame's lights: Key_ShadowCast
    // lights first, then the casters nearest the camera, each fading over
    // kPointShadowFadeSeconds as it gains or loses its slot.
    void UpdatePointShadows(std::span<const model::FrameState::LightState> lights,
                            const Vector3f& cameraWS, f32 dtSeconds);
    // Live slots, packed from 0.
    std::span<const PointShadow> PointShadows() const {
        return {pointShadows_.data(), static_cast<usize>(pointShadowCount_)};
    }
    // The slot whose light sits at `worldPos`, or -1.
    i32 PointShadowSlotFor(const Vector3f& worldPos) const;
    gfx::TextureHandle PointShadowArray() const {
        return pointShadowArray_;
    }
    // Slots the shadow pass filled this frame, counted from 0.
    void SetRenderedPointShadows(i32 n) {
        renderedPointShadows_ = n;
    }
    i32 RenderedPointShadows() const {
        return renderedPointShadows_;
    }

private:
    void EnsureTargets();
    void DestroyTargets();

    static Matrix44f BuildLightViewLH(const Vector3f& center, const Vector3f& lightDir,
                                      f32 casterHeight);

    static Matrix44f OrthoLHOffcenter(f32 l, f32 r, f32 b, f32 t, f32 n, f32 f);

    Matrix44f CameraSliceProjLH(const Matrix44f& camProj, f32 cameraNearZ, f32 cameraFarZ,
                                f32 nearSplit, f32 farSplit) const;

    static void FrustumCornersWS(const Matrix44f& view, const Matrix44f& proj,
                                 Vector3f outCorners[8]);

    gfx::IGFXDevice* gfx_ = nullptr;
    ShadowParams params_;
    gfx::TextureHandle depthArray_ = gfx::TextureHandle::Invalid;
    i32 resolution_ = 0;
    std::array<Matrix44f, kMaxCascades> worldToClip_{};
    i32 renderedCascades_ = 0;
    Matrix44f lightView_ = Matrix44f::identity();
    bool targetsValid_ = false;

    // A caster the allocator is tracking, matched across frames by position.
    struct TrackedCaster {
        Vector3f position;
        f32 nearZ;
        f32 farZ;
        f32 strength;
        bool priority;
        bool wanted;
    };
    std::vector<TrackedCaster> tracked_;
    std::array<PointShadow, kPointShadowSlots> pointShadows_{};
    i32 pointShadowCount_ = 0;
    i32 renderedPointShadows_ = 0;
    gfx::TextureHandle pointShadowArray_ = gfx::TextureHandle::Invalid;
};

} // namespace whiteout::flakes::renderer::shadow
