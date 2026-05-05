#include "bls_frame.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace WhiteoutDex::bls {

namespace {

inline f32 AlphaRefFor(GxMatAlpha a) {
    switch (a) {
        case GxMatAlpha::AlphaKey:   return 192.0f / 255.0f;
        case GxMatAlpha::Blend:
        case GxMatAlpha::Add:
        case GxMatAlpha::Modulate:
        case GxMatAlpha::Modulate2X: return 4.0f / 255.0f;
        case GxMatAlpha::Opaque:
        default:                     return 0.0f;
    }
}

}

void BuildSdVsCbA(SdVsCbA& out, const FrameInputs& in, const MatParams& mat) {

    const Matrix44f wv  = in.world * in.view;
    const Matrix44f wvp = wv * in.projection;

    out.world         = wv;
    out.worldViewProj = wvp;
    out.diffuseColor  = mat.diffuseColor;
    out.texMtx0       = in.texMtx0;
    out.texMtx1       = in.texMtx1;

    const i32 n = std::clamp(in.numLights, 0, kMaxLights);
    for (i32 i = 0; i < n; ++i) out.lights[i] = in.lights[i];
    for (i32 i = n; i < kMaxLights; ++i) out.lights[i] = {};
}

void BuildSdPsCbA(SdPsCbA& out, const FrameInputs& in, const MatParams& mat) {
    std::memset(&out, 0, sizeof(out));
    out.alphaRef  = AlphaRefFor(mat.alpha);
    out.fogParams = in.fogParams;
    out.fogColor  = in.fogColor;
}

void BuildHdVsCb(HdVsCb& out, const FrameInputs& in, const MatParams& mat) {

    const Matrix44f wv  = in.world * in.view;
    const Matrix44f wvp = wv * in.projection;

    out.world         = in.world;
    out.worldView     = wv;
    out.worldViewProj = wvp;

    out.misc          = { in.effectTime, mat.popcornScale, 0.0f, 0.0f };
    out.diffuseColor  = mat.diffuseColor;
    out.texMtx0       = in.texMtx0;
    out.texMtx1       = in.texMtx1;
}

void BuildHdPsCb(HdPsCb& out, const FrameInputs& in, const MatParams& mat) {
    std::memset(&out, 0, sizeof(out));
    out.alphaRef     = AlphaRefFor(mat.alpha);
    out.fogParams    = in.fogParams;
    out.fogColor     = in.fogColor;
    out.worldView    = in.world * in.view;
    out.view         = in.view;
    out.projection   = in.projection;
    out.viewportRect = in.viewportRect;
    out.effectTime   = in.effectTime;
    out.emissiveGain = mat.emissiveGain;

    const i32 nLights = std::clamp(in.numLights, 0, kMaxLights);
    const u32 countBits = static_cast<u32>(nLights);
    std::memcpy(&out.lightCount, &countBits, sizeof(f32));

    out.useNdf       = in.useNdf ? 1.0f : 0.0f;

    out.pixelParams1 = { mat.inverseSoftness,
                         mat.cloakAmount,
                         mat.fresnelTeamColor,
                         0.0f };

    out.fresnelColor = { mat.fresnelColor.x,
                         mat.fresnelColor.y,
                         mat.fresnelColor.z,
                         mat.fresnelOpacity };

    out.envMapParams = { in.envFromMipEnd, in.envToMipEnd, in.envTransitionT, 0.0f };

    for (i32 i = 0; i < nLights; ++i) {
        out.lights[i] = in.lights[i];
        out.lights[i]._pad = { 0.25f, 0.0f, 0.0f, 0.0f };
    }
    for (i32 i = nLights; i < kMaxLights; ++i) out.lights[i] = {};
}

void BuildSdOnHdPsCb(SdOnHdPsCb& out, const FrameInputs& in, const MatParams& mat) {
    std::memset(&out, 0, sizeof(out));
    out.alphaRef  = AlphaRefFor(mat.alpha);
    out.fogParams = in.fogParams;
    out.fogColor  = in.fogColor;

    const Matrix44f invView = Matrix44f::inverse(in.view);
    out.invViewRow0 = { invView.data[0][0], invView.data[0][1], invView.data[0][2], invView.data[0][3] };
    out.invViewRow1 = { invView.data[1][0], invView.data[1][1], invView.data[1][2], invView.data[1][3] };
    out.invViewRow2 = { invView.data[2][0], invView.data[2][1], invView.data[2][2], invView.data[2][3] };

    out.pixelParams1 = {1.0f, 0.0f, 0.0f, 0.0f};
    out.envMapParams = { in.envFromMipEnd, in.envToMipEnd, in.envTransitionT, 0.0f };

    const i32 n = std::clamp(in.numLights, 0, kMaxLights);
    const u32 countBits = static_cast<u32>(n);
    f32 countAsFloat;
    std::memcpy(&countAsFloat, &countBits, sizeof(f32));
    out.lightCountSlot = {0, 0, countAsFloat, 0};

    for (i32 i = 0; i < n; ++i) out.lights[i] = in.lights[i];
    for (i32 i = n; i < kMaxLights; ++i) out.lights[i] = {};
}

void PackBone(ShaderBone& out, const Matrix44f& m) {
    const Matrix44f t = m.transpose();
    out.row0 = { t.data[0][0], t.data[0][1], t.data[0][2], t.data[0][3] };
    out.row1 = { t.data[1][0], t.data[1][1], t.data[1][2], t.data[1][3] };
    out.row2 = { t.data[2][0], t.data[2][1], t.data[2][2], t.data[2][3] };
}

void BuildBonePalette(BonePaletteCb& out, const Matrix44f* src, i32 numBones) {
    PackBone(out.bones[0], Matrix44f::identity());
    const i32 n = std::clamp(numBones, 0, kMaxBones);
    for (i32 i = 0; i < n; ++i) PackBone(out.bones[i], src[i]);
    for (i32 i = std::max(1, n); i < kMaxBones; ++i) {
        PackBone(out.bones[i], Matrix44f::identity());
    }
}

ShaderTexMtx ComposeTexAnimMatrix(const Quaternion& q,
                                  const Vector3f&   s,
                                  const Vector3f&   t) {

    const f32 ang = 2.0f * std::atan2(q.z, q.w);
    const f32 c   = std::cos(ang);
    const f32 si  = std::sin(ang);

    const f32 a =  s.x * c;
    const f32 b = -s.x * si;
    const f32 d =  s.y * si;
    const f32 e =  s.y * c;

    const f32 cc = 0.5f - (a * 0.5f + b * 0.5f) + t.x;
    const f32 ff = 0.5f - (d * 0.5f + e * 0.5f) + t.y;

    ShaderTexMtx m{};
    m.rows[0] = { a, b, 0.0f, cc };
    m.rows[1] = { d, e, 0.0f, ff };
    return m;
}

void PackBoneVertex(BoneVertex& out, const i32 indices[4], const f32 weights[4]) {
    f32 ws[4]; f32 total = 0;
    for (i32 i = 0; i < 4; ++i) {
        f32 w = (weights[i] > 0 && std::isfinite(weights[i])) ? weights[i] : 0.0f;
        ws[i] = w; total += w;
    }
    if (total > 1e-6f) {
        f32 scale = 255.0f / total;
        i32   acc = 0, last = 0;
        for (i32 i = 0; i < 4; ++i) {
            i32 q = std::clamp(static_cast<i32>(std::lround(ws[i] * scale)), 0, 255);
            out.weights[i] = static_cast<u8>(q);
            acc += q; if (q > 0) last = i;
        }
        if (acc != 255) {
            i32 adj = std::clamp(static_cast<i32>(out.weights[last]) + (255 - acc), 0, 255);
            out.weights[last] = static_cast<u8>(adj);
        }
    } else {
        out.weights[0] = 255;
        out.weights[1] = 0; out.weights[2] = 0; out.weights[3] = 0;
    }
    for (i32 i = 0; i < 4; ++i) {
        i32 idx = std::clamp(indices[i], 0, 255);
        out.indices[i] = static_cast<u8>(idx);
    }
}

}
