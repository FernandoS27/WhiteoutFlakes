// ============================================================================
// WhiteoutFlakes Renderer — Unlit: position-only, flat white.
//
// The shader half of UnlitShading (REFACTOR_PLAN.md P8). Deliberately an
// *engine* shader on the embedded slangc path rather than a BLS one: engine
// shaders never enter a `.bls` container, `bls::PsoRequest` requires a
// `const BlsProgram*`, and producing a new bundle needs WDX_BUILD_WC3_SHADERS,
// which is mutually exclusive with the WDX_USE_PREBUILT_SHADERS that CI and
// every fresh clone build with. So this compiles alongside blit / line / gtao
// and lands in compiled_shaders.h.
//
// Its own constant buffer, not common.slang's PerFrameData: that struct mirrors
// C++ CBPerFrame field for field and carries eight vectors of lighting state
// this shader has no use for. Three matrices is the whole contract.
//
// The vertex input declares POSITION only, while the bound buffer is WC3's
// fully interleaved 48-byte Vertex. That is intentional and legal — the input
// layout names one element at offset 0 and passes the full stride, so the
// remaining 36 bytes per vertex are simply not read. This is what "position-
// only vertex stream" means without de-interleaving anything.
// ============================================================================

struct UnlitData
{
    float4x4 world;
    float4x4 view;
    float4x4 projection;
}

ConstantBuffer<UnlitData> cb;

struct VSInput
{
    float3 position : POSITION;
}

struct PSInput
{
    float4 position : SV_POSITION;
}

// ---- Vertex Shader ----
//
// Row-vector convention against column-major storage, matching line.slang and
// every BLS shader: the C++ side transposes before upload, so the multiply
// order here is position × world × view × projection.

[shader("vertex")]
PSInput vsUnlit(VSInput input)
{
    PSInput output;

    let worldPos = mul(float4(input.position, 1.0), cb.world);
    let viewPos  = mul(worldPos, cb.view);
    output.position = mul(viewPos, cb.projection);

    return output;
}

// ---- Pixel Shader ----
//
// Writes SV_Target0 only. In the HD G-buffer pass the pipeline binds three
// attachments; the backend masks writes to the slots a shader does not declare,
// so the cleared linear-depth and normal values survive — the same arrangement
// the debug line PSO already relies on.

[shader("fragment")]
float4 psUnlit(PSInput input) : SV_Target
{
    return float4(1.0, 1.0, 1.0, 1.0);
}

// ============================================================================
// Lit permutations — POSITION + NORMAL.
//
// These exist to prove a MeshBuffer's layout description. `.m2` and `.m3` hand
// over an interleaved blob plus a table saying where each attribute lives; the
// renderer uploads it without decoding. Nothing about that is checkable while
// the shader reads position alone — a normal at the wrong offset, in the wrong
// format, or silently absent all render the same flat white. Lighting makes the
// description visible, which is the entire reason it is here.
//
// The vertex/pixel split is by *axis*: one VS for "this buffer has a normal",
// two PS for which profile is drawing. The flat pair above is untouched and is
// still what a geoset without a normal falls back to.
//
// Lighting state is a SECOND constant buffer rather than more fields on
// UnlitData, so the flat permutation's CB layout — and therefore its compiled
// bytecode — does not move at all.
// ============================================================================

struct UnlitLightData
{
    float4 lightDirWS;   // .xyz = direction TO the light, normalised. .w unused.
    float4 lightColor;   // .rgb = radiance. .w unused.
    float4 ambient;      // .rgb = ambient term. .w unused.
    float4 specular;     // .rgb = specular tint, .w = Blinn-Phong exponent.
    float4 cameraPosWS;  // .xyz = eye position in world space. .w unused.
}

ConstantBuffer<UnlitLightData> light;

struct PSInputLit
{
    float4 position : SV_POSITION;
    float3 normalWS : NORMAL;
    float3 worldPos : TEXCOORD0;
}

struct VSInputLit
{
    float3 position : POSITION;
    float3 normal   : NORMAL;
}

// Normals ride the world matrix directly rather than its inverse-transpose.
// Correct here and not a shortcut worth apologising for: the world transform on
// these actors is a rotation, a translation and a uniform WorldScale, and
// uniform scale leaves normal direction unchanged after the renormalise below.
// A non-uniform scale would need the inverse transpose, and nothing in this
// path produces one.
[shader("vertex")]
PSInputLit vsUnlitLit(VSInputLit input)
{
    PSInputLit output;

    let worldPos = mul(float4(input.position, 1.0), cb.world);
    let viewPos  = mul(worldPos, cb.view);
    output.position = mul(viewPos, cb.projection);
    output.worldPos = worldPos.xyz;
    output.normalWS = mul(float4(input.normal, 0.0), cb.world).xyz;

    return output;
}

// Two-sided: a bare `dot` would leave every back-facing triangle black, and the
// unlit path draws with culling off. Flipping the normal toward the viewer
// keeps a back face lit like the front one instead of reading as a hole.
float3 FaceForward(float3 n, float3 worldPos, float3 eye)
{
    let v = normalize(eye - worldPos);
    return dot(n, v) < 0.0 ? -n : n;
}

[shader("fragment")]
float4 psUnlitLambert(PSInputLit input) : SV_Target
{
    let n = FaceForward(normalize(input.normalWS), input.worldPos, light.cameraPosWS.xyz);
    let ndotl = max(dot(n, light.lightDirWS.xyz), 0.0);
    let color = light.ambient.rgb + light.lightColor.rgb * ndotl;
    return float4(color, 1.0);
}

// ============================================================================
// Skinned permutations — the same two pixel shaders, posed by a bone palette.
//
// `.m3` carries BLENDWEIGHT and BLENDINDICES inside its own vertex blob (at
// offsets 12 and 16), already coalesced for the GPU, and stores each index as a
// slot into that region's bone palette rather than a global bone id. So there
// is nothing to decode and no second vertex stream: the same slot-0 buffer that
// supplies position and normal supplies the skinning attributes too, and the
// palette the renderer binds is the region's own.
//
// The palette layout is byte-identical to the one WC3's skinned geosets bind —
// bls::PackBone's transposed 4x3, three float4 rows per bone, 256 slots. Flat
// float4 array for the same reason M2's is: nothing to get wrong across four
// backends.
// ============================================================================

struct UnlitBoneData
{
    float4 rows[768];
}

ConstantBuffer<UnlitBoneData> bones;

struct VSInputSkinned
{
    float3 position    : POSITION;
    float3 normal      : NORMAL;
    float4 boneWeights : BLENDWEIGHT;
    uint4  boneIndices : BLENDINDICES;
}

// One bone's transform, rebuilt from its three packed rows.
float4x4 BoneMatrix(uint slot)
{
    let b = slot * 3;
    let r0 = bones.rows[b + 0];
    let r1 = bones.rows[b + 1];
    let r2 = bones.rows[b + 2];
    // Undo the transpose PackBone applied, back to the row-vector form the
    // rest of this shader multiplies with.
    return float4x4(
        float4(r0.x, r1.x, r2.x, 0.0),
        float4(r0.y, r1.y, r2.y, 0.0),
        float4(r0.z, r1.z, r2.z, 0.0),
        float4(r0.w, r1.w, r2.w, 1.0));
}

// Linear blend skinning. Weights arrive UNORM-decoded and are used as authored
// — no renormalise, because a deliberately under-weighted vertex is a thing
// artists do and rescaling it would be the renderer overruling the asset.
float4x4 SkinMatrix(float4 w, uint4 idx)
{
    float4x4 m = BoneMatrix(idx.x) * w.x;
    m += BoneMatrix(idx.y) * w.y;
    m += BoneMatrix(idx.z) * w.z;
    m += BoneMatrix(idx.w) * w.w;
    return m;
}

[shader("vertex")]
PSInput vsUnlitSkinned(VSInputSkinned input)
{
    PSInput output;

    let skin = SkinMatrix(input.boneWeights, input.boneIndices);
    let modelPos = mul(float4(input.position, 1.0), skin);
    let worldPos = mul(modelPos, cb.world);
    let viewPos  = mul(worldPos, cb.view);
    output.position = mul(viewPos, cb.projection);

    return output;
}

[shader("vertex")]
PSInputLit vsUnlitSkinnedLit(VSInputSkinned input)
{
    PSInputLit output;

    let skin = SkinMatrix(input.boneWeights, input.boneIndices);
    let modelPos = mul(float4(input.position, 1.0), skin);
    let worldPos = mul(modelPos, cb.world);
    let viewPos  = mul(worldPos, cb.view);
    output.position = mul(viewPos, cb.projection);
    output.worldPos = worldPos.xyz;
    // Through the skin matrix as well as the world one, or every normal stays
    // in bind pose while the geometry moves.
    let skinnedNormal = mul(float4(input.normal, 0.0), skin).xyz;
    output.normalWS = mul(float4(skinnedNormal, 0.0), cb.world).xyz;

    return output;
}

[shader("fragment")]
float4 psUnlitBlinnPhong(PSInputLit input) : SV_Target
{
    let n = FaceForward(normalize(input.normalWS), input.worldPos, light.cameraPosWS.xyz);
    let l = light.lightDirWS.xyz;
    let v = normalize(light.cameraPosWS.xyz - input.worldPos);
    let h = normalize(l + v);

    let ndotl = max(dot(n, l), 0.0);
    let ndoth = max(dot(n, h), 0.0);
    // Gated on ndotl so no highlight appears on a surface facing away from
    // the light, which is what separates Blinn-Phong from "Lambert plus a
    // stripe" on the silhouette.
    let spec = (ndotl > 0.0) ? pow(ndoth, max(light.specular.w, 1.0)) : 0.0;

    let color = light.ambient.rgb
              + light.lightColor.rgb * ndotl
              + light.specular.rgb * spec;
    return float4(color, 1.0);
}
