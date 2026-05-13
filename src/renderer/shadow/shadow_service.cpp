#include "renderer/shadow/shadow_service.h"

#include "whiteout/flakes/types.h"

#include <algorithm>
#include <cmath>

namespace whiteout::flakes::renderer::shadow {

namespace {

Vector3f MulPoint(const Matrix44f& m, const Vector3f& v) {
    const f32 x = v.x * m.data[0][0] + v.y * m.data[1][0] + v.z * m.data[2][0] + m.data[3][0];
    const f32 y = v.x * m.data[0][1] + v.y * m.data[1][1] + v.z * m.data[2][1] + m.data[3][1];
    const f32 z = v.x * m.data[0][2] + v.y * m.data[1][2] + v.z * m.data[2][2] + m.data[3][2];
    const f32 w = v.x * m.data[0][3] + v.y * m.data[1][3] + v.z * m.data[2][3] + m.data[3][3];
    const f32 invW = (std::abs(w) > 1.0e-6f) ? (1.0f / w) : 1.0f;
    return {x * invW, y * invW, z * invW};
}

} // namespace

ShadowService::ShadowService(gfx::IGFXDevice* gfx) : gfx_(gfx) {}

ShadowService::~ShadowService() {
    DestroyTargets();
}

void ShadowService::SetParams(const ShadowParams& p) {
    const bool needsRealloc =
        p.cascadeResolution != params_.cascadeResolution || p.cascadeCount != params_.cascadeCount;
    params_ = p;
    if (needsRealloc) {
        DestroyTargets();
        targetsValid_ = false;
    }
}

void ShadowService::SetEnabled(bool on) {
    params_.enabled = on;
}

gfx::TextureHandle ShadowService::depthTarget(i32 c) const {
    if (c < 0 || c >= 3)
        return gfx::TextureHandle::Invalid;
    return cascades_[c].depth;
}

const Matrix44f& ShadowService::cascadeVP(i32 c) const {
    if (c < 0 || c >= 3) {
        static const Matrix44f kIdentity = Matrix44f::identity();
        return kIdentity;
    }
    return cascades_[c].worldToClip;
}

void ShadowService::EnsureTargets() {
    if (targetsValid_)
        return;
    if (!gfx_)
        return;
    const i32 res = std::max(64, params_.cascadeResolution);
    for (i32 c = 0; c < 3; ++c) {
        if (cascades_[c].depth != gfx::TextureHandle::Invalid && cascades_[c].resolution == res) {
            continue;
        }
        if (cascades_[c].depth != gfx::TextureHandle::Invalid) {
            gfx_->Destroy(cascades_[c].depth);
        }

        gfx::TextureDesc td{};
        td.width = res;
        td.height = res;
        td.mipLevels = 1;
        td.format = gfx::Format::D32_FLOAT;
        td.usage = gfx::TextureUsage::DepthStencil | gfx::TextureUsage::ShaderResource;
        cascades_[c].depth = gfx_->CreateTexture(td, nullptr);
        cascades_[c].resolution = res;
        cascades_[c].worldToClip = Matrix44f::identity();
    }
    targetsValid_ = true;
}

void ShadowService::DestroyTargets() {
    if (!gfx_)
        return;
    for (auto& c : cascades_) {
        if (c.depth != gfx::TextureHandle::Invalid) {
            gfx_->Destroy(c.depth);
            c.depth = gfx::TextureHandle::Invalid;
        }
        c.resolution = 0;
    }
}

Matrix44f ShadowService::BuildLightViewLH(const Vector3f& center, const Vector3f& lightDirIn,
                                          f32 casterHeight) {

    Vector3f L = lightDirIn;
    f32 n2 = L.x * L.x + L.y * L.y + L.z * L.z;
    if (n2 < 1.0e-12f) {
        L = {0.0f, 0.0f, -1.0f};
    } else {
        const f32 invLen = 1.0f / std::sqrt(n2);
        L = {L.x * invLen, L.y * invLen, L.z * invLen};
    }

    const Vector3f eye = {
        center.x - L.x * casterHeight,
        center.y - L.y * casterHeight,
        center.z - L.z * casterHeight,
    };

    const Vector3f worldUp =
        (std::abs(L.z) < 0.95f) ? Vector3f{0.0f, 0.0f, 1.0f} : Vector3f{0.0f, 1.0f, 0.0f};
    return Matrix44f::look_at_lh_sgcompat(eye, center, worldUp);
}

Matrix44f ShadowService::OrthoLHOffcenter(f32 l, f32 r, f32 b, f32 t, f32 n, f32 f) {

    Matrix44f m{};
    const f32 dx = r - l;
    const f32 dy = t - b;
    const f32 dz = f - n;
    m.data[0][0] = 2.0f / dx;
    m.data[1][1] = 2.0f / dy;
    m.data[2][2] = 1.0f / dz;
    m.data[3][0] = -(r + l) / dx;
    m.data[3][1] = -(t + b) / dy;
    m.data[3][2] = -n / dz;
    m.data[3][3] = 1.0f;
    return m;
}

Matrix44f ShadowService::CameraSliceProjLH(const Matrix44f& camProj, f32 cameraNearZ,
                                           f32 cameraFarZ, f32 nearSplit, f32 farSplit) const {

    Matrix44f m = camProj;
    const f32 n = std::max(cameraNearZ, nearSplit);
    const f32 f = std::min(cameraFarZ, farSplit);
    if (f <= n)
        return m;
    m.data[2][2] = f / (f - n);
    m.data[3][2] = -n * f / (f - n);
    return m;
}

void ShadowService::FrustumCornersWS(const Matrix44f& view, const Matrix44f& proj,
                                     Vector3f outCorners[8]) {

    static const Vector3f kNdc[8] = {
        {-1.0f, -1.0f, 0.0f}, {1.0f, -1.0f, 0.0f}, {-1.0f, 1.0f, 0.0f}, {1.0f, 1.0f, 0.0f},
        {-1.0f, -1.0f, 1.0f}, {1.0f, -1.0f, 1.0f}, {-1.0f, 1.0f, 1.0f}, {1.0f, 1.0f, 1.0f},
    };
    Matrix44f viewProj = view * proj;
    Matrix44f invVP = Matrix44f::inverse(viewProj);
    for (i32 i = 0; i < 8; ++i) {
        outCorners[i] = MulPoint(invVP, kNdc[i]);
    }
}

void ShadowService::Update(const Matrix44f& cameraViewLH, const Matrix44f& cameraProjLH,
                           f32 cameraNearZ, f32 cameraFarZ, const Vector3f& lightDirWS,
                           const Vector3f& sceneCenterWS, f32 sceneRadius) {
    if (!params_.enabled)
        return;
    EnsureTargets();
    if (!targetsValid_)
        return;

    lightView_ = BuildLightViewLH(sceneCenterWS, lightDirWS, params_.casterHeight);

    Vector3f sceneAabbMinLS = {3.4e38f, 3.4e38f, 3.4e38f};
    Vector3f sceneAabbMaxLS = {-3.4e38f, -3.4e38f, -3.4e38f};
    {
        const f32 r = std::max(sceneRadius, 1.0f);
        const Vector3f sceneCornersWS[8] = {
            {sceneCenterWS.x - r, sceneCenterWS.y - r, sceneCenterWS.z - r},
            {sceneCenterWS.x + r, sceneCenterWS.y - r, sceneCenterWS.z - r},
            {sceneCenterWS.x - r, sceneCenterWS.y + r, sceneCenterWS.z - r},
            {sceneCenterWS.x + r, sceneCenterWS.y + r, sceneCenterWS.z - r},
            {sceneCenterWS.x - r, sceneCenterWS.y - r, sceneCenterWS.z + r},
            {sceneCenterWS.x + r, sceneCenterWS.y - r, sceneCenterWS.z + r},
            {sceneCenterWS.x - r, sceneCenterWS.y + r, sceneCenterWS.z + r},
            {sceneCenterWS.x + r, sceneCenterWS.y + r, sceneCenterWS.z + r},
        };
        for (const auto& p : sceneCornersWS) {
            const Vector3f ls = MulPoint(lightView_, p);
            sceneAabbMinLS.x = std::min(sceneAabbMinLS.x, ls.x);
            sceneAabbMaxLS.x = std::max(sceneAabbMaxLS.x, ls.x);
            sceneAabbMinLS.y = std::min(sceneAabbMinLS.y, ls.y);
            sceneAabbMaxLS.y = std::max(sceneAabbMaxLS.y, ls.y);
            sceneAabbMinLS.z = std::min(sceneAabbMinLS.z, ls.z);
            sceneAabbMaxLS.z = std::max(sceneAabbMaxLS.z, ls.z);
        }
    }

    const i32 N = std::clamp(params_.cascadeCount, 1, 3);
    const f32 zNear = std::max(cameraNearZ, 1.0f);
    const f32 farCap = std::max(zNear + 1.0f, sceneRadius * 4.0f);
    const f32 zFar = std::min(std::max(zNear + 1.0f, cameraFarZ), farCap);
    const f32 ratio = zFar / zNear;
    f32 splits[4] = {zNear, 0.0f, 0.0f, zFar};
    for (i32 i = 0; i < N - 1; ++i) {
        const f32 u = static_cast<f32>(i + 1) / static_cast<f32>(N);
        const f32 zU = zNear + (zFar - zNear) * u;
        const f32 zL = zNear * std::pow(ratio, u);
        splits[i + 1] = zU + (zL - zU) * params_.lambdaSplit;
    }
    splits[N] = zFar;

    for (i32 c = 0; c < N; ++c) {
        const f32 nSlice = splits[c];
        const f32 fSlice = splits[c + 1];

        const Matrix44f sliceProj =
            CameraSliceProjLH(cameraProjLH, cameraNearZ, cameraFarZ, nSlice, fSlice);

        Vector3f cornersWS[8];
        FrustumCornersWS(cameraViewLH, sliceProj, cornersWS);

        Vector3f minLS = {3.4e38f, 3.4e38f, 3.4e38f};
        Vector3f maxLS = {-3.4e38f, -3.4e38f, -3.4e38f};
        for (i32 i = 0; i < 8; ++i) {
            const Vector3f p = MulPoint(lightView_, cornersWS[i]);
            minLS.x = std::min(minLS.x, p.x);
            maxLS.x = std::max(maxLS.x, p.x);
            minLS.y = std::min(minLS.y, p.y);
            maxLS.y = std::max(maxLS.y, p.y);
            minLS.z = std::min(minLS.z, p.z);
            maxLS.z = std::max(maxLS.z, p.z);
        }

        minLS.x = std::max(minLS.x, sceneAabbMinLS.x);
        maxLS.x = std::min(maxLS.x, sceneAabbMaxLS.x);
        minLS.y = std::max(minLS.y, sceneAabbMinLS.y);
        maxLS.y = std::min(maxLS.y, sceneAabbMaxLS.y);

        minLS.z = std::min(minLS.z, sceneAabbMinLS.z - 50.0f);
        maxLS.z = std::max(maxLS.z, sceneAabbMaxLS.z + 50.0f);

        const bool degenerate =
            (maxLS.x <= minLS.x) || (maxLS.y <= minLS.y) || (maxLS.z <= minLS.z);
        if (degenerate) {
            minLS = sceneAabbMinLS;
            maxLS = sceneAabbMaxLS;
            minLS.z -= 50.0f;
            maxLS.z += 50.0f;

            const f32 kMinExtent = 1.0f;
            if (maxLS.x - minLS.x < kMinExtent) {
                const f32 c = 0.5f * (minLS.x + maxLS.x);
                minLS.x = c - 0.5f * kMinExtent;
                maxLS.x = c + 0.5f * kMinExtent;
            }
            if (maxLS.y - minLS.y < kMinExtent) {
                const f32 c = 0.5f * (minLS.y + maxLS.y);
                minLS.y = c - 0.5f * kMinExtent;
                maxLS.y = c + 0.5f * kMinExtent;
            }
            if (maxLS.z - minLS.z < kMinExtent) {
                const f32 c = 0.5f * (minLS.z + maxLS.z);
                minLS.z = c - 0.5f * kMinExtent;
                maxLS.z = c + 0.5f * kMinExtent;
            }
        }

        if (params_.texelSnap && cascades_[c].resolution > 0) {
            const f32 worldUnitsPerTexelX =
                (maxLS.x - minLS.x) / static_cast<f32>(cascades_[c].resolution);
            const f32 worldUnitsPerTexelY =
                (maxLS.y - minLS.y) / static_cast<f32>(cascades_[c].resolution);
            if (worldUnitsPerTexelX > 0.0f && worldUnitsPerTexelY > 0.0f) {
                minLS.x = std::floor(minLS.x / worldUnitsPerTexelX) * worldUnitsPerTexelX;
                maxLS.x = std::ceil(maxLS.x / worldUnitsPerTexelX) * worldUnitsPerTexelX;
                minLS.y = std::floor(minLS.y / worldUnitsPerTexelY) * worldUnitsPerTexelY;
                maxLS.y = std::ceil(maxLS.y / worldUnitsPerTexelY) * worldUnitsPerTexelY;
            }
        }

        const Matrix44f cascadeProj =
            OrthoLHOffcenter(minLS.x, maxLS.x, minLS.y, maxLS.y, minLS.z, maxLS.z);

        cascades_[c].worldToClip = lightView_ * cascadeProj;
    }

    for (i32 c = N; c < 3; ++c) {
        cascades_[c].worldToClip = Matrix44f::identity();
    }
}

} // namespace whiteout::flakes::renderer::shadow
