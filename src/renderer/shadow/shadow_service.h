#pragma once

#include "whiteout/flakes/types.h"
#include "whiteout/flakes/shadow_params.h"
#include "renderer/types.h"
#include "gfx/gfx.h"

#include <array>

namespace whiteout::flakes::renderer::shadow {

struct Cascade {
    gfx::TextureHandle depth         = gfx::TextureHandle::Invalid;
    Matrix44f          worldToClip   = Matrix44f::identity();
    i32                resolution    = 0;
};

class ShadowService {
public:
    explicit ShadowService(gfx::IGFXDevice* gfx);
    ~ShadowService();

    ShadowService(const ShadowService&) = delete;
    ShadowService& operator=(const ShadowService&) = delete;

    void                SetParams(const ShadowParams& p);
    const ShadowParams& Params() const { return params_; }

    void  SetEnabled(bool on);
    bool  IsEnabled() const { return params_.enabled; }

    void Update(const Matrix44f& cameraViewLH,
                const Matrix44f& cameraProjLH,
                f32              cameraNearZ,
                f32              cameraFarZ,
                const Vector3f&  lightDirWS,
                const Vector3f&  sceneCenterWS,
                f32              sceneRadius);

    i32                cascadeCount() const { return params_.cascadeCount; }
    gfx::TextureHandle depthTarget (i32 c) const;
    const Matrix44f&   cascadeVP   (i32 c) const;

    template <typename HdShadowCascadesCb>
    void FillVsCb(HdShadowCascadesCb& out) const {
        out.cascade0 = cascadeVP(0);
        out.cascade1 = cascadeVP(1);
        out.cascade2 = cascadeVP(2);
    }

private:
    void EnsureTargets();
    void DestroyTargets();

    static Matrix44f BuildLightViewLH(const Vector3f& center, const Vector3f& lightDir,
                                      f32 casterHeight);

    static Matrix44f OrthoLHOffcenter(f32 l, f32 r, f32 b, f32 t, f32 n, f32 f);

    Matrix44f CameraSliceProjLH(const Matrix44f& camProj,
                                f32              cameraNearZ,
                                f32              cameraFarZ,
                                f32              nearSplit,
                                f32              farSplit) const;

    static void FrustumCornersWS(const Matrix44f& view, const Matrix44f& proj,
                                 Vector3f outCorners[8]);

    gfx::IGFXDevice*               gfx_ = nullptr;
    ShadowParams                   params_;
    std::array<Cascade, 3>         cascades_{};
    Matrix44f                      lightView_ = Matrix44f::identity();
    bool                           targetsValid_ = false;
};

}
