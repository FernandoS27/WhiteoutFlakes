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

// CGxDevice::m_sdOnHDLightCompensationValue, initialised to 0.3f at the tail of
// CGxDevice::CGxDevice (Engine/Source/Gx/CGxDevice.cpp). IStateSync passes it to
// CGxLightToShaderLight as ambLightModifier, but only for GxShaderID_SD_ON_HD;
// every other shader gets 0.
inline constexpr f32 kSdOnHdAmbientCompensation = 0.3f;

// `ps/sd_on_hd.bls` reads lights[0]._pad.x (register 24) outside the light
// loop, as a blend weight in the team-colour block:
//     r6.y = hasTeamTex * (hasTeamTex + max(lights[0].diffuse.rgb)) * pad.x
// and then lerps the team-colour result against the base by r6.y.
//
// The engine-side source of this value is unknown: the disassembled client
// (Warcraft IIId.exe) predates the 64-byte ShaderLight and has no fourth
// vec4 to write. 0 leaves the term inert, which is the behaviour we shipped
// before — do not change it to a guess without a reference capture to
// compare against.
inline constexpr f32 kSdOnHdLight0BlendWeight = 0.0f;

// `ps/hd.bls` reads the same register, and there it is *not* inert — it scales
// the only NdotL-independent term in the lighting tail:
//     r3.w  = iblRan * (iblRan + max(lights[0].diffuse.rgb)) * pad.x
//     r12   = r3.w * irradiance
//     r9    = saturate(dot(N,L)) * r9 + r12          <-- r12 survives at NdotL=0
//     r9   += lights[0].ambient
// so pad.x is what lets the env-probe irradiance reach an unlit surface. At 0
// a back-facing pixel gets `lights[0].ambient` and nothing else, which on the
// HD DNC rigs (ambientIntensity = 0) means pure black, and on the SD rigs
// means a flat 0.3 with no probe contribution at all.
//
// Verified with BlsReflect over ps/hd.bls: register 24 is read by perms
// 8, 9, 12, 13, 24 and 25 — every lit, non-prepass permutation, including
// perm 9, the team-coloured unit perm. (Perms with lights=0 or prepass=1
// have no lighting tail, which is why a spot-check of those reads nothing.)
inline constexpr f32 kHdLight0BlendWeight = 0.15f;

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
}

void BuildSdPsCbA(SdPsCbA& out, const FrameInputs& in, const MatParams& mat) {
    std::memset(&out, 0, sizeof(out));
    out.alphaRef = ResolveAlphaRef(mat);
    out.fogParams = in.fogParams;
    out.fogColor = in.fogColor;
}

void BuildHdVsCb(HdVsCb& out, const FrameInputs& in, const MatParams& mat) {

    const Matrix44f wv = in.world * in.view;
    const Matrix44f wvp = wv * in.projection;

    out.world = in.world;
    out.worldView = wv;
    out.worldViewProj = wvp;

    out.misc = {in.effectTime, mat.cornEffectsScale, 0.0f, 0.0f};
    out.diffuseColor = mat.diffuseColor;
    out.texMtx0 = in.texMtx0;
    out.texMtx1 = in.texMtx1;
}

void BuildHdPsCb(HdPsCb& out, const FrameInputs& in, const MatParams& mat) {
    std::memset(&out, 0, sizeof(out));
    out.alphaRef = ResolveAlphaRef(mat);
    out.fogParams = in.fogParams;
    out.fogColor = in.fogColor;
    out.worldView = in.world * in.view;
    out.view = in.view;
    out.projection = in.projection;
    out.viewportRect = in.viewportRect;
    out.effectTime = in.effectTime;
    out.emissiveGain = mat.emissiveGain;

    const i32 nLights = std::clamp(in.numLights, 0, kMaxLights);
    const u32 countBits = static_cast<u32>(nLights);
    std::memcpy(&out.lightCount, &countBits, sizeof(f32));

    out.useNdf = in.useNdf ? 1.0f : 0.0f;

    out.pixelParams1 = {mat.inverseSoftness, mat.cloakAmount, mat.fresnelTeamColor, 0.0f};

    out.fresnelColor = {mat.fresnelColor.x, mat.fresnelColor.y, mat.fresnelColor.z,
                        mat.fresnelOpacity};

    out.envMapParams = {in.envFromMipEnd, in.envToMipEnd, in.envTransitionT, 0.0f};

    for (i32 i = 0; i < nLights; ++i)
        out.lights[i] = in.lights[i];
    for (i32 i = nLights; i < kMaxLights; ++i)
        out.lights[i] = {};

    // Only light 0's slot is read (register 24); see kHdLight0BlendWeight.
    if (nLights > 0)
        out.lights[0]._pad = {kHdLight0BlendWeight, 0.0f, 0.0f, 0.0f};
}

void BuildSdOnHdPsCb(SdOnHdPsCb& out, const FrameInputs& in, const MatParams& mat) {
    std::memset(&out, 0, sizeof(out));
    out.alphaRef = ResolveAlphaRef(mat);
    out.fogParams = in.fogParams;
    out.fogColor = in.fogColor;

    const Matrix44f invView = Matrix44f::inverse(in.view);
    out.invViewRow0 = {invView.data[0][0], invView.data[0][1], invView.data[0][2],
                       invView.data[0][3]};
    out.invViewRow1 = {invView.data[1][0], invView.data[1][1], invView.data[1][2],
                       invView.data[1][3]};
    out.invViewRow2 = {invView.data[2][0], invView.data[2][1], invView.data[2][2],
                       invView.data[2][3]};

    out.pixelParams1 = {1.0f, 0.0f, 0.0f, 0.0f};
    out.envMapParams = {in.envFromMipEnd, in.envToMipEnd, in.envTransitionT, 0.0f};

    const i32 n = std::clamp(in.numLights, 0, kMaxLights);
    const u32 countBits = static_cast<u32>(n);
    f32 countAsFloat;
    std::memcpy(&countAsFloat, &countBits, sizeof(f32));
    out.lightCountSlot = {0, 0, countAsFloat, 0};

    // SD-on-HD is the one path where CGxLightToShaderLight's ambLightModifier
    // is non-zero, so it is the only place the MDX ambient colour (KLBC)
    // contributes: ambient = ambIntensity + modifier * ambColor.
    for (i32 i = 0; i < n; ++i) {
        const Vector3f& ac = in.lightAmbientColors[i];
        out.lights[i] = in.lights[i];
        out.lights[i].ambient.x += kSdOnHdAmbientCompensation * ac.x;
        out.lights[i].ambient.y += kSdOnHdAmbientCompensation * ac.y;
        out.lights[i].ambient.z += kSdOnHdAmbientCompensation * ac.z;
    }
    for (i32 i = n; i < kMaxLights; ++i)
        out.lights[i] = {};

    if (n > 0)
        out.lights[0]._pad = {kSdOnHdLight0BlendWeight, 0.0f, 0.0f, 0.0f};
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
