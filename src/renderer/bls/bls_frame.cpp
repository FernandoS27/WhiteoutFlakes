#include "bls_frame.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

namespace whiteout::flakes::renderer::bls {

namespace {

inline f32 AlphaRefFor(GxMatAlpha a) {
    switch (a) {
    case GxMatAlpha::AlphaKey:
        return kAlphaKeyRef;
    case GxMatAlpha::Blend:
    case GxMatAlpha::Add:
    case GxMatAlpha::Modulate:
    case GxMatAlpha::Modulate2X:
        return 4.0f / 255.0f;
    case GxMatAlpha::Opaque:
    default:
        return 0.0f;
    }
}

// Explicit per-material override (set when promoting an alpha-key layer to a
// blend) wins; otherwise derive from the blend mode.
inline f32 ResolveAlphaRef(const MatParams& mat) {
    return mat.alphaRef >= 0.0f ? mat.alphaRef : AlphaRefFor(mat.alpha);
}

} // namespace

void BuildSdVsCbA(SdVsCbA& out, const FrameInputs& in, const MatParams& mat) {

    const Matrix44f wv = in.world * in.view;
    const Matrix44f wvp = wv * in.projection;

    out.world = wv;
    out.worldViewProj = wvp;
    out.diffuseColor = mat.diffuseColor;
    out.texMtx0 = in.texMtx0;
    out.texMtx1 = in.texMtx1;

    const i32 n = std::clamp(in.numLights, 0, kMaxLights);
    for (i32 i = 0; i < n; ++i)
        out.lights[i] = in.lights[i];
    for (i32 i = n; i < kMaxLights; ++i)
        out.lights[i] = {};

    // The SD PS fogs on TEXCOORD1.z, which the engine's left-handed view makes
    // the distance ahead of the camera. The SD passes draw with the right-handed
    // camera, where that z is negative and every fog mode reads "no fog". Turn
    // the view space half a turn about Y, the difference between the two
    // cameras; the lights turn with it, so the per-vertex lighting is unchanged.
    if (in.projection.data[2][3] < 0.0f) {
        for (auto& row : out.world.data) {
            row[0] = -row[0];
            row[2] = -row[2];
        }
        for (i32 i = 0; i < n; ++i) {
            out.lights[i].position.x = -out.lights[i].position.x;
            out.lights[i].position.z = -out.lights[i].position.z;
        }
    }
}

f32 EngineFogLinear(f32 x) {
    return x * (x * (0.305f * x + 0.682f) + 0.0125f);
}

i32 DrawFogMode(const FogParams& fog, const MatParams& mat) {
    return mat.FogEnabled() ? std::clamp(fog.mode, 0, 6) : 0;
}

void BuildSdPsCbA(SdPsCbA& out, const FrameInputs& in, const MatParams& mat) {
    std::memset(&out, 0, sizeof(out));
    out.alphaRef = ResolveAlphaRef(mat);
    const FogParams& f = in.fog;
    out.fogColorStart = {f.colorSrgb.x, f.colorSrgb.y, f.colorSrgb.z, f.start};
    out.fogEnd = f.end;
    out.fogDensity = f.density;
}

void BuildHdVsCb(HdVsCb& out, const FrameInputs& in, const MatParams& mat) {
    std::memset(&out, 0, sizeof(out));
    const Matrix44f wv = in.world * in.view;

    out.world = in.world;
    out.worldView = wv;
    out.worldViewProj = wv * in.projection;
    out.projection = in.projection;
    out.effectTime = in.effectTime;
    out.popcornScale = mat.cornEffectsScale;
    // clipHeight / underWater stay 0: no water clip plane, so the PS never
    // discards on it (and the depth prepass would, on anything but 0).
    out.boneBufferBase = IntBits(in.boneBufferBase);
    out.diffuseColor = mat.diffuseColor;
    out.texMtx0 = in.texMtx0;
    out.texMtx1 = in.texMtx1;
}

i32 EngineBlendMode(GxMatAlpha alpha) {
    switch (alpha) {
    case GxMatAlpha::Opaque:
        return 0;
    case GxMatAlpha::AlphaKey:
        return 1;
    case GxMatAlpha::Blend:
    case GxMatAlpha::BlendKeepDst:
        return 2;
    case GxMatAlpha::Add:
    case GxMatAlpha::AddNoAlpha:
        return 3;
    case GxMatAlpha::Modulate:
        return 4;
    case GxMatAlpha::Modulate2X:
        return 5;
    case GxMatAlpha::PremulBlend:
        return 6;
    }
    return 0;
}

void BuildHdPsCb(HdPsCb& out, const FrameInputs& in, const MatParams& mat) {
    std::memset(&out, 0, sizeof(out));
    out.alphaRef = ResolveAlphaRef(mat);
    out.blendMode = IntBits(EngineBlendMode(mat.alpha));

    const FogParams& f = in.fog;
    out.fogColorStart = {EngineFogLinear(f.colorSrgb.x), EngineFogLinear(f.colorSrgb.y),
                         EngineFogLinear(f.colorSrgb.z), f.start};
    out.fogEnd = f.end;
    out.fogDensity = f.density;
    out.heightTop = f.heightTop;
    out.heightBottom = f.heightBottom;
    out.radialInner = f.radialInner;
    out.radialOuter = f.radialOuter;
    out.radialStrength = f.radialStrength;
    out.fogEverywhere = IntBits(f.everywhere ? 1 : 0);

    const Matrix44f invView = Matrix44f::inverse(in.view);
    out.worldView = in.world * in.view;
    out.invView = invView;
    out.invProjection = Matrix44f::inverse(in.projection);
    out.projection = in.projection;
    // The volumetric fog centre is the camera: (0,0,0,1) through the inverse view.
    out.fogCentre = {invView.data[3][0], invView.data[3][1], invView.data[3][2], 1.0f};
    out.depthUVRemap = {1.0f, 1.0f, 0.0f, 0.0f};

    // Row 22.x is written 0/1 from flag 0x400 for the HD programs, and the PS
    // multiplies its output alpha by it — every material draw has it set.
    out.outputAlphaScale = 1.0f;
    out.cloakAmount = mat.cloakAmount;
    out.fresnelTeamColor = mat.fresnelTeamColor;
    out.fresnelColor = {mat.fresnelColor.x, mat.fresnelColor.y, mat.fresnelColor.z,
                        mat.fresnelOpacity};

    out.envFromMipCount = in.envFromMipCount;
    out.envToMipCount = in.envToMipCount;
    out.envTransitionT = in.envTransitionT;

    out.effectTime = in.effectTime;
    out.emissiveGain = mat.emissiveGain;
    // GetShadowSettings()+65 picks 0.01 or 0.0001; only the cube shadows read it.
    out.shadowDepthBias = 0.0001f;
    out.useNdf = IntBits(in.useNdf ? 1 : 0);
    // GetNormalStrengthSetting() defaults to 1.
    out.normalStrength = 1.0f;
    out.fogMode = IntBits(DrawFogMode(in.fog, mat));

    const MainLight& L = in.mainLight;
    const bool lit = L.enabled && mat.LightingEnabled();
    out.mainLightEnable = IntBits(lit ? 1 : 0);
    out.ambient = {L.ambient.x, L.ambient.y, L.ambient.z, L.shadowIntensity};
    out.lightColor = {L.color.x, L.color.y, L.color.z, 0.0f};
    out.lightDirVS = {L.dirToLightVS.x, L.dirToLightVS.y, L.dirToLightVS.z, 0.0f};
}

void PackBone(ShaderBone& out, const Matrix44f& m) {
    const Matrix44f t = m.transpose();
    out.row0 = {t.data[0][0], t.data[0][1], t.data[0][2], t.data[0][3]};
    out.row1 = {t.data[1][0], t.data[1][1], t.data[1][2], t.data[1][3]};
    out.row2 = {t.data[2][0], t.data[2][1], t.data[2][2], t.data[2][3]};
}

void BuildBonePalette(BonePaletteCb& out, const Matrix44f* src, i32 numBones) {
    // Only write the n real bones. Skipping the (kMaxBones - n) padding
    // saves ~12 KB of memory writes per call; on PE1-heavy scenes
    // (1015+ calls / frame) that's the bulk of UpdateAnimation's cost.
    //
    // Invariant we rely on: every vertex's bone-index attribute is in
    // [0, numBones). Valid WC3 MDX data satisfies this — bone indices
    // out of range would already be broken regardless of what's in the
    // unused palette slots. The CB ring rotates mapped slots, so stale
    // data from a previous frame's actor could appear in those slots;
    // a malformed vertex referencing them would read garbage and
    // produce visible glitches. If you ever see swimming geometry on
    // a specific model, that model is the canary — re-enable padding
    // here (single std::memcpy from a precomputed kIdentityBlock) and
    // open an issue against the offending asset.
    //
    // bones[0] stays the identity-fallback for vertices that opt out
    // of skinning entirely (boneIdx==0, weight==1). When numBones==0
    // we still want bones[0] populated so those vertices draw at
    // local origin instead of NaN-land.
    if (numBones <= 0) {
        PackBone(out.bones[0], Matrix44f::identity());
        return;
    }
    const i32 n = std::min(numBones, kMaxBones);
    for (i32 i = 0; i < n; ++i)
        PackBone(out.bones[i], src[i]);
}

ShaderTexMtx ComposeTexAnimMatrix(const Quaternion& q, const Vector3f& s, const Vector3f& t) {
    // WC3 AnimateTextureMap: uv' = ((uv + t - 0.5) * S) * R + 0.5, A = R*S with
    // CCW rotation. See the live build in mdx_model_adapter's texAnimMatrices.
    const f32 ang = 2.0f * std::atan2(q.z, q.w);
    const f32 c = std::cos(ang), si = std::sin(ang);

    const f32 a = s.x * c;
    const f32 b = -s.y * si;
    const f32 d = s.x * si;
    const f32 e = s.y * c;

    const f32 px = t.x - 0.5f, py = t.y - 0.5f;
    ShaderTexMtx m{};
    m.rows[0] = {a, b, 0.0f, a * px + b * py + 0.5f};
    m.rows[1] = {d, e, 0.0f, d * px + e * py + 0.5f};
    return m;
}

void PackBoneVertex(BoneVertex& out, const i32 indices[4], const f32 weights[4]) {
    f32 ws[4];
    f32 total = 0;
    for (i32 i = 0; i < 4; ++i) {
        f32 w = (weights[i] > 0 && std::isfinite(weights[i])) ? weights[i] : 0.0f;
        ws[i] = w;
        total += w;
    }
    if (total > 1e-6f) {
        f32 scale = 255.0f / total;
        i32 acc = 0, last = 0;
        for (i32 i = 0; i < 4; ++i) {
            i32 q = std::clamp(static_cast<i32>(std::lround(ws[i] * scale)), 0, 255);
            out.weights[i] = static_cast<u8>(q);
            acc += q;
            if (q > 0)
                last = i;
        }
        if (acc != 255) {
            i32 adj = std::clamp(static_cast<i32>(out.weights[last]) + (255 - acc), 0, 255);
            out.weights[last] = static_cast<u8>(adj);
        }
    } else {
        out.weights[0] = 255;
        out.weights[1] = 0;
        out.weights[2] = 0;
        out.weights[3] = 0;
    }
    for (i32 i = 0; i < 4; ++i) {
        i32 idx = std::clamp(indices[i], 0, 255);
        out.indices[i] = static_cast<u8>(idx);
    }
}

} // namespace whiteout::flakes::renderer::bls
