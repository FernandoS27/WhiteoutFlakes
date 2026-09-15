#include "renderer/profiles/diablo3/d3_particle_shading.h"

#include "compiled_shaders.h"
#include "io/d3/d3_sno_cache.h"
#include "io/d3/d3_types.h"
#include "renderer/assets/sampler_asset_manager.h"
#include "renderer/assets/texture_asset_manager.h"
#include "renderer/model/model_instance.h"
#include "renderer/profiles/diablo3/d3_standard_shading.h"
#include "renderer/profiles/diablo3/d3_surface_table.h"
#include "renderer/render_pipeline.h"
#include "renderer/render_service.h"

#include <whiteout/sno/d3/native/types.h>

#include <algorithm>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>

namespace whiteout::flakes::renderer::profiles::diablo3 {

namespace d3n = ::whiteout::sno::d3::native;

namespace {

/// @brief The blend class the shared draw list carries.
///
/// A lossy summary, and knowingly so: the D3 path binds `blendSrc` / `blendDst`
/// straight onto the pipeline, and this exists for the fallback that draws a
/// D3 particle through the WC3 SD program when the D3 one is unavailable.
/// ENGINE blend numbering, from the shipped passes — (5, 6) on 115 of the
/// corpus's 223 `Billboard.fx` shaders and (5, 2) on 54.
particle::FilterMode BlendClassOf(const pd3::MaterialDesc& m) {
    if (!m.blendEnable)
        return m.alphaTest > 0.0f ? particle::FilterMode::AlphaKey : particle::FilterMode::Blend;
    // A premultiplied pass is an over-composite or an add depending on what its
    // program writes in the alpha, and both leave the destination alone where
    // the sprite is empty — which is the half this summary has to get right.
    if (m.pmaMode != 0)
        return m.pmaMode == 2 ? particle::FilterMode::Additive : particle::FilterMode::Blend;
    if (m.blendDst == 2 /*ONE*/)
        return particle::FilterMode::Additive;
    if (m.blendSrc == 7 /*DESTCOLOR*/ || m.blendDst == 3 /*SRCCOLOR*/)
        return particle::FilterMode::Modulate;
    return particle::FilterMode::Blend;
}


/// @brief One layer's step in a combine chain the PROGRAM fixes rather than the
///        pass, for the entry points whose passes carry no stage block at all.
struct FixedStage {
    u8 colorOp;
    u8 alphaOp;
    f32 colorGain;
    f32 alphaGain;
};

/// @brief What a `szPixelShaderEntry` says the program does.
///
/// Read off the shipped bytecode of each entry point (D3_MATERIAL_AUDIT.md
/// S12), not inferred: the four `ps_firewall_*` programs differ only in two
/// literals, and the name is the whole of what selects between them.
struct ProgramShape {
    io::D3ParticleProgram program = io::D3ParticleProgram::Chain;
    /// The first layer that is a flow map, or -1.
    i32 flowFirst = -1;
    bool softFade = false;
    /// Null where the pass's own stage block is the authority.
    const FixedStage* fixed = nullptr;
};

// `alpha = 2 * layer19.a * (vcol.a * diffuse.a) * layer12.a` and
// `rgb = vcol.rgb * diffuse.rgb * layer12.rgb` -- the second layer feeds the
// alpha only, and the trailing gain is the program's, not any stage's.
constexpr FixedStage kFirewall[3] = {{io::kD3StageModulate, io::kD3StageModulate, 1.0f, 1.0f},
                                     {io::kD3StageSkip, io::kD3StageModulate, 1.0f, 1.0f},
                                     {io::kD3StageModulate, io::kD3StageModulate, 1.0f, 2.0f}};
/// `_am4x` does not replace that 2 -- it multiplies it, and the shipped
/// program's literal is 8.
constexpr FixedStage kFirewallAm4x[3] = {{io::kD3StageModulate, io::kD3StageModulate, 1.0f, 1.0f},
                                         {io::kD3StageSkip, io::kD3StageModulate, 1.0f, 1.0f},
                                         {io::kD3StageModulate, io::kD3StageModulate, 1.0f, 8.0f}};
/// `_cm2x_am4x` also stops dropping the second layer's colour.
constexpr FixedStage kFirewallCm2xAm4x[3] = {
    {io::kD3StageModulate, io::kD3StageModulate, 1.0f, 1.0f},
    {io::kD3StageModulate, io::kD3StageModulate, 1.0f, 1.0f},
    {io::kD3StageModulate, io::kD3StageModulate, 2.0f, 8.0f}};
/// `ps_billboard_cm2x_flow`: two diffuse layers and the x2 on the colour only.
constexpr FixedStage kCm2xFlow[2] = {{io::kD3StageModulate, io::kD3StageModulate, 1.0f, 1.0f},
                                     {io::kD3StageModulate, io::kD3StageModulate, 2.0f, 1.0f}};

ProgramShape D3ProgramShapeOf(std::string_view entry) {
    ProgramShape sh;
    if (entry == "ps_firewall_flow" || entry == "ps_firewall_flow_pma") {
        sh.flowFirst = 3;
        sh.fixed = kFirewall;
    } else if (entry == "ps_firewall_am4x_flow") {
        sh.flowFirst = 3;
        sh.fixed = kFirewallAm4x;
    } else if (entry == "ps_firewall_cm2x_am4x_flow") {
        sh.flowFirst = 3;
        sh.fixed = kFirewallCm2xAm4x;
    } else if (entry == "ps_billboard_cm2x_flow") {
        sh.flowFirst = 2;
        sh.fixed = kCm2xFlow;
    } else if (entry == "ps_legacy_flow") {
        sh.flowFirst = 2;
    } else if (entry == "ps_particle_flow") {
        sh.flowFirst = 1;
    } else if (entry == "ps_billboard_blendAdd_flowMult") {
        sh.program = io::D3ParticleProgram::BlendAdd;
        sh.flowFirst = 2;
    } else if (entry == "ps_blend_add_pma") {
        sh.program = io::D3ParticleProgram::BlendAdd;
    } else if (entry == "ps_blend_add_soft_particle") {
        sh.program = io::D3ParticleProgram::BlendAdd;
        sh.softFade = true;
    } else if (entry == "ps_particle_water_sim") {
        sh.program = io::D3ParticleProgram::WaterSim;
    }
    return sh;
}

} // namespace

void D3ResolveParticleMaterial(const d3n::Particle& prt, ::whiteout::flakes::io::D3SnoCache* cache,
                               pd3::MaterialDesc& out) {
    // The frame table, for EVERY layer and not only the uv mode 3 ones. The
    // engine reaches it through the STAGE's own texture -- the `.an2` the entry
    // names is only ever consulted for the loop mode -- so this asks the `.tex`,
    // which owes the ShaderMap chain nothing and must therefore sit ABOVE the
    // early-out below: 58 of the first 400 systems resolve no pass.
    //
    // Every layer, because the sheet decides two things a mode-3 test would
    // miss. `Particle_WriteQuadVertices` takes the quad's BASE RECTANGLE and its
    // vertical aspect off **stage 0's** sheet whenever that sheet has frames,
    // whatever uv mode stage 0's entry carries -- the mode only picks the
    // transform applied on top. Every stage gets its own flip-book player, so
    // there is no single "atlas layer" to choose: 505 shipped materials carry
    // two mode-3 stages and 5 carry three, and each walks its own sheet.
    if (cache)
        for (u32 i = 0; i < out.layerCount; ++i)
            out.layers[i].atlas = cache->TextureAtlas(out.layers[i].textureSno);
    // The positional slot map, which the compacted layer array cannot stand in
    // for: `Particle_BindDrawTextures` parks type 1 in slot 0 and type 14 in
    // slot 3 whatever else is present.
    for (u32 i = 0; i < out.layerCount; ++i)
        for (u32 k = 0; k < pd3::MaterialDesc::kMaxLayers; ++k)
            if (out.layers[i].rawType == io::kD3TexcoordSetType[k])
                out.setLayer[k] = static_cast<i8>(i);

    const D3PassState pass = D3PassStateFor(prt.tMaterial, cache);
    if (!pass.resolved)
        return;

    out.passResolved = true;
    out.distortion = pass.distortion;
    out.blendEnable = pass.blendEnable;
    out.blendSrc = pass.blendSrc;
    out.blendDst = pass.blendDst;
    out.depthWrite = pass.depthWrite;
    out.depthFunc = pass.depthFunc;
    out.pmaMode = pass.pmaMode;
    out.colorWrite = pass.colorWrite;
    out.alphaWrite = pass.alphaWrite;
    out.depthBias = pass.depthBias;
    out.alphaFunc = pass.alphaTestEnable ? pass.alphaFunc : 0u;
    out.alphaTest = pass.alphaTestEnable ? static_cast<f32>(pass.alphaRef) / 255.0f : 0.0f;
    out.effectFile = pass.effectFile;
    out.pixelEntry = pass.pixelEntry;

    // Which program the pass names, and where its flow maps sit. A flow shader
    // reads its LAST texcoord slots as distortion rather than as colour, and
    // how many of them are live is the pass's own texcoord count -- the same
    // tag that says a slot exists at all.
    ProgramShape shape = D3ProgramShapeOf(pass.pixelEntry);
    // ...except for the one arm the entry name cannot name. A `.shd` ships one
    // compiled permutation, so five `ps_legacy` passes are really `ps_legacy`
    // built with BLENDADD; what says so is the hole they leave in their own
    // stage block. See D3PassState::stageHole. Scoped to the billboard
    // families because a hole means this only where the program addresses its
    // units positionally: 34 `Legacy.fx` passes leave one and none is a sum.
    if (shape.program == io::D3ParticleProgram::Chain && pass.stageHole &&
        (pass.effectFile == "Billboard.fx" || pass.effectFile == "SoftBillboard.fx"))
        shape.program = io::D3ParticleProgram::BlendAdd;
    // Every one of these programs addresses its textures by SLOT -- t0 is the
    // diffuse, t3 the flow map -- while this layer list is COMPACTED to the
    // types the material actually carries. They agree only while the types are
    // a prefix of the bind order, so a material missing one of them keeps the
    // chain rather than reading its alpha mask as a flow map.
    {
        const u32 need = std::min<u32>(out.layerCount, pd3::MaterialDesc::kMaxLayers);
        bool aligned = true;
        for (u32 i = 0; aligned && i < need; ++i)
            aligned = out.layers[i].rawType == io::kD3TexcoordSetType[i];
        if (!aligned)
            shape = ProgramShape{};
    }
    if (shape.program == io::D3ParticleProgram::BlendAdd && out.layerCount < 2)
        shape = ProgramShape{};
    out.program = shape.program;
    out.softFade = shape.softFade;
    if (shape.flowFirst >= 0) {
        const i32 last =
            static_cast<i32>(std::min<u32>(pass.texcoordCount, out.layerCount)) - 1;
        // An unbound layer samples white, and white through the flow formula
        // is a constant quarter-tile shift rather than no shift at all -- so a
        // material that names no texture for its flow slot warps by nothing.
        bool bound = true;
        for (i32 i = shape.flowFirst; i <= last; ++i)
            bound = bound && out.layers[i].textureSno >= 0;
        if (last >= shape.flowFirst && bound) {
            out.flowFirst = shape.flowFirst;
            out.flowLast = last;
        }
    }

    // The address modes, which live nowhere else: `Particle_DrawBatch` binds a
    // texture id and no sampler state at all, so a particle's wrap is whatever
    // its pass declares for that stage. The `.prt` entry's flags word is not
    // it -- see io/d3/d3_types.h -- and reading it there left 140 of the
    // corpus's 493 wrapping billboard stages on clamp, which smears a scrolling
    // layer's edge row across the whole sprite.
    for (u32 i = 0; i < out.layerCount; ++i)
        out.layers[i].wrapFlags = pass.WrapBitsFor(out.layers[i].rawType);

    // Which of the vertex's four texcoords each stage samples at. The vertex
    // program's tag block says, and it is not always the stage's own: the code
    // names a uv SET, and the sets are POSITIONAL over the stage types with a
    // hole where a type is absent -- which is why the answer is a set index and
    // not an index into this compacted array. A slot past the pass's texcoord
    // count keeps its own set.
    for (u32 i = 0; i < out.layerCount; ++i) {
        u32 own = 0;
        for (u32 k = 0; k < pd3::MaterialDesc::kMaxLayers; ++k)
            if (out.layers[i].rawType == io::kD3TexcoordSetType[k])
                own = k;
        out.layers[i].uvSet = own;
        if (i < pass.texcoordCount)
            out.layers[i].uvSet = io::D3TexcoordUvSet(pass.texcoordFunc[i]);
    }

    // A program whose combine is its own: the firewall family carries no stage
    // block at all on the shaders the corpus's `.prt` reach, and the two
    // `cm2x` / `am4x` gains are literals in the bytecode rather than tags.
    if (shape.fixed) {
        const u32 n = out.flowFirst >= 0 ? static_cast<u32>(out.flowFirst) : out.layerCount;
        for (u32 i = 0; i < out.layerCount; ++i) {
            pd3::MaterialLayer& L = out.layers[i];
            if (i >= n) {
                L.colorOp = io::kD3StageSkip;
                L.alphaOp = io::kD3StageSkip;
                continue;
            }
            L.colorOp = shape.fixed[i].colorOp;
            L.alphaOp = shape.fixed[i].alphaOp;
            L.colorGain = shape.fixed[i].colorGain;
            L.alphaGain = shape.fixed[i].alphaGain;
            L.colorClamp = false;
            L.alphaClamp = false;
        }
        return;
    }
    if (!pass.stageArgs)
        return;
    out.erosion = pass.erosion;
    out.colorVcolFirst = pass.colorVcolFirst;
    out.colorVcolLast = pass.colorVcolLast;
    out.alphaVcolFirst = pass.alphaVcolFirst;
    out.alphaVcolLast = pass.alphaVcolLast;
    // The pass's combine chain, matched to the layers by TYPE. A layer whose
    // type the pass declares no stage for is not bound in the original at all
    // and its texture is never sampled -- 3,242 of the corpus's 17,503 systems,
    // led by the 1,064 that hand `particle_additive` (one stage) an alpha mask
    // it has nowhere to put.
    for (u32 i = 0; i < out.layerCount; ++i) {
        pd3::MaterialLayer& L = out.layers[i];
        const D3PassState::Combine* cb = nullptr;
        for (u32 k = 0; k < pass.combineCount; ++k) {
            if (pass.combines[k].type == L.rawType) {
                cb = &pass.combines[k];
                break;
            }
        }
        if (!cb) {
            L.colorOp = ::whiteout::flakes::io::kD3StageSkip;
            L.alphaOp = ::whiteout::flakes::io::kD3StageSkip;
            continue;
        }
        L.colorOp = cb->colorOp;
        L.alphaOp = cb->alphaOp;
        L.colorGain = cb->colorGain;
        L.alphaGain = cb->alphaGain;
        L.colorClamp = cb->colorClamp;
        L.alphaClamp = cb->alphaClamp;
    }
    // A flow map is not a combine stage, whatever the block parks on its slot:
    // five of the seven `ps_legacy_flow` passes tag theirs `3`, which as a
    // combine code would REPLACE the sprite with the noise texture.
    for (i32 i = out.flowFirst; i >= 0 && i <= out.flowLast; ++i) {
        out.layers[i].colorOp = io::kD3StageSkip;
        out.layers[i].alphaOp = io::kD3StageSkip;
    }
}

void D3BindParticleTextures(model::Actor& actor, const std::shared_ptr<pd3::EmitterDesc>& desc) {
    if (!desc)
        return;
    pd3::MaterialDesc& m = desc->d3mat;

    for (u32 i = 0; i < m.layerCount; ++i) {
        pd3::MaterialLayer& L = m.layers[i];
        if (L.textureSno < 0)
            continue;
        auto [it, fresh] = actor.d3ParticleTextures.try_emplace(
            L.textureSno,
            kD3ParticleTextureIdBase + static_cast<i32>(actor.d3ParticleTextures.size()));
        L.textureId = it->second;
        if (!fresh)
            continue;
        model::StagedTexture& st = actor.render.stagedTextures[L.textureId];
        // "#<snoId>" is the form UploadStagedTextures parses into an
        // id-addressed ContentRef; a scheme-prefixed key would be read as a
        // path and leave the layer white with no error anywhere.
        st.sharedKey = "#" + std::to_string(L.textureSno);
        st.wrapFlags = L.wrapFlags;
        actor.render.stagedDirty = true;
    }

    desc->material.textureId = m.DiffuseTextureId();
    desc->material.filterMode = BlendClassOf(m);
    desc->material.unshaded = true;
    desc->material.unfogged = false;
    // Aliasing: the chain stays inside the desc the emitter holds, so the draw
    // list carries a refcount rather than four layers and a string.
    desc->material.d3 = std::shared_ptr<const pd3::MaterialDesc>(desc, &desc->d3mat);
}

// ---------------------------------------------------------------------------
// The draw
// ---------------------------------------------------------------------------

namespace {

struct D3ParticleVsCb {
    Matrix44f view;
    Matrix44f projection;
    /// .x the pass's depth bias; .yzw spare. See D3PassState::depthBias.
    Vector4f params;
};

/// @brief The vertex this program takes: an ordinary billboard plus the FOUR
///        baked texture coordinates the engine's own particle vertex carries.
///
/// `Particle_WriteQuadVertices` writes a 56-byte vertex whose +20/+24/+28/+32
/// are four packed texcoords, one per stage, each already transformed. It bakes
/// them because the transform is per PARTICLE — see BuildGeometryInput::d3Uv01.
/// Full floats rather than the engine's 16-bit pairs: the packing is a memory
/// economy this build does not need, and a half-precision coordinate on a
/// 64-tile sheet loses the tile.
struct D3ParticleVertex {
    Vector3f position;
    Vector4f color;
    Vector4f uv01; ///< set 0 in .xy, set 1 in .zw
    Vector4f uv23;
    f32 color1; ///< the whole of the engine's second D3DCOLOR — ch6, all four lanes equal
};
static_assert(sizeof(D3ParticleVertex) == 64, "D3ParticleVertex must stay tightly packed");

struct D3ParticlePsCb {
    /// Per layer: .x which of the vertex's four baked texcoords this stage
    /// samples at, .y the COLOUR op, .z the ALPHA op, .w spare. There is no
    /// transform here any more — it is per particle and rides the vertex.
    Vector4f stage[pd3::MaterialDesc::kMaxLayers];
    /// Per layer: .x colour gain, .y alpha gain, .z/.w the two clamps.
    Vector4f gains[pd3::MaterialDesc::kMaxLayers];
    /// .x the vertex-colour placement bits, .y the program, .z the alpha-test
    /// reference, .w spare.
    Vector4f params;
    /// .x the premultiplied-alpha output mode; .y the alpha-test comparison;
    /// .z the first flow layer or -1; .w the last flow layer.
    Vector4f params2;
    /// .x the erosion exponent, or 0 for no dissolve tail; .yzw spare.
    Vector4f params3;
};

/// Everything that can vary between two of these draws, in one key. The
/// attachment formats are in it because a PSO belongs to the pass it runs in
/// and the transparent pass is MRT in HD and single-target in SD.
u64 PsoKeyOf(const pd3::MaterialDesc& m, const D3ParticleFrameInputs& f) {
    auto fmt = [](gfx::Format v) { return static_cast<u64>(v) & 0xFFull; };
    u64 k = m.blendEnable ? 1ull : 0ull;
    k |= static_cast<u64>(m.blendSrc & 0x1F) << 1;
    k |= static_cast<u64>(m.blendDst & 0x1F) << 6;
    k |= (m.depthWrite ? 1ull : 0ull) << 11;
    // The compare is pipeline state, and 8 means the pass draws with no depth
    // test at all — which is what every premultiplied particle asks for.
    k |= static_cast<u64>(m.depthFunc & 0xF) << 54;
    k |= (m.colorWrite ? 1ull : 0ull) << 58;
    k |= (m.alphaWrite ? 1ull : 0ull) << 59;
    k |= fmt(f.rtvFormat) << 12;
    k |= fmt(f.dsvFormat) << 20;
    k |= static_cast<u64>(f.extraRtvCount & 0x3) << 28;
    k |= fmt(f.extraRtvFormats[0]) << 30;
    k |= fmt(f.extraRtvFormats[1]) << 38;
    k |= fmt(f.extraRtvFormats[2]) << 46;
    return k;
}

} // namespace

void D3ParticleShading::Init() {
    if (initTried_)
        return;
    auto* gfxDev = rs_.Pipeline().Gfx();
    if (!gfxDev)
        return;
    initTried_ = true;

    using namespace whiteout::flakes::Shaders;
    const gfx::GfxApi api = gfxDev->GetApi();
    auto mk = [&](gfx::ShaderStage stage, const u8* dxbc, usize dxbcN, const u8* spv, usize spvN,
                  const u8* wgsl, usize wgslN, const u8* mtl, usize mtlN) {
        switch (api) {
        case gfx::GfxApi::Vulkan:
            return gfxDev->CreateShader(stage, spv, spvN);
        case gfx::GfxApi::WebGPU:
            return gfxDev->CreateShader(stage, wgsl, wgslN);
        case gfx::GfxApi::Metal:
            return gfxDev->CreateShader(stage, mtl, mtlN);
        default:
            return gfxDev->CreateShader(stage, dxbc, dxbcN);
        }
    };
#define WDX_D3P_BLOB(name)                                                                         \
    k##name, sizeof(k##name), k##name##Spv, sizeof(k##name##Spv), k##name##Wgsl,                   \
        sizeof(k##name##Wgsl), k##name##Mtl, sizeof(k##name##Mtl)
    vs_ = mk(gfx::ShaderStage::Vertex, WDX_D3P_BLOB(D3ParticleVS));
    ps_ = mk(gfx::ShaderStage::Pixel, WDX_D3P_BLOB(D3ParticlePS));
#undef WDX_D3P_BLOB

    // Written once per frame; every draw in it shares the transforms.
    vsCb_ = gfxDev->CreateBuffer({
        .size = sizeof(D3ParticleVsCb),
        .usage = gfx::BufferUsage::Constant | gfx::BufferUsage::CpuWritable,
    });
    // Mapped once per DRAW — the UV affines move per emitter — so this one
    // needs the deep ring every hot per-draw CB gets.
    gfx::BufferDesc pd;
    pd.size = sizeof(D3ParticlePsCb);
    pd.usage = gfx::BufferUsage::Constant | gfx::BufferUsage::CpuWritable;
    pd.ringSlotsHint = 4096;
    psCb_ = gfxDev->CreateBuffer(pd);
}

void D3ParticleShading::ReleaseGpu() {
    auto* gfxDev = rs_.Pipeline().Gfx();
    if (!gfxDev)
        return;
    for (auto& [k, pso] : psos_) {
        if (pso != gfx::PipelineHandle::Invalid)
            gfxDev->Destroy(pso);
    }
    psos_.clear();
    for (gfx::BufferHandle* b : {&vsCb_, &psCb_, &vb_}) {
        if (*b != gfx::BufferHandle::Invalid)
            gfxDev->Destroy(*b);
        *b = gfx::BufferHandle::Invalid;
    }
    vbCapacity_ = 0;
    frameReady_ = false;
    vs_ = gfx::ShaderHandle::Invalid;
    ps_ = gfx::ShaderHandle::Invalid;
    initTried_ = false;
}

bool D3ParticleShading::BeginFrame(const std::vector<Vertex>& vertices,
                                   const particle::D3VertexStream& uv) {
    frameReady_ = false;
    auto* gfxDev = rs_.Pipeline().Gfx();
    if (!gfxDev || vertices.empty())
        return false;
    // A length mismatch means an emitter reached the shared stream without its
    // texcoords; packing anyway would read whatever the last frame left there.
    if (uv.uv01.size() != vertices.size() || uv.uv23.size() != vertices.size() ||
        uv.color1.size() != vertices.size())
        return false;

    const i32 count = static_cast<i32>(vertices.size());
    if (vb_ == gfx::BufferHandle::Invalid || count > vbCapacity_) {
        if (vb_ != gfx::BufferHandle::Invalid)
            gfxDev->Destroy(vb_);
        constexpr i32 kFloor = 4096;
        const i32 newSize = (count > kFloor) ? count : kFloor;
        gfx::BufferDesc bd;
        bd.size = static_cast<u64>(sizeof(D3ParticleVertex)) * static_cast<u64>(newSize);
        bd.usage = gfx::BufferUsage::Vertex | gfx::BufferUsage::CpuWritable;
        bd.ringSlotsHint = 4; // mapped once per frame
        vb_ = gfxDev->CreateBuffer(bd);
        vbCapacity_ = newSize;
    }
    if (vb_ == gfx::BufferHandle::Invalid)
        return false;
    void* mapped = gfxDev->MapBuffer(vb_);
    if (!mapped)
        return false;
    auto* dst = static_cast<D3ParticleVertex*>(mapped);
    for (i32 i = 0; i < count; ++i) {
        const usize k = static_cast<usize>(i);
        dst[i].position = vertices[k].position;
        dst[i].color = vertices[k].color;
        dst[i].uv01 = uv.uv01[k];
        dst[i].uv23 = uv.uv23[k];
        dst[i].color1 = uv.color1[k];
    }
    gfxDev->UnmapBuffer(vb_);
    frameReady_ = true;
    return true;
}

bool D3ParticleShading::IsAvailable() const {
    return vs_ != gfx::ShaderHandle::Invalid && ps_ != gfx::ShaderHandle::Invalid &&
           vsCb_ != gfx::BufferHandle::Invalid && psCb_ != gfx::BufferHandle::Invalid;
}

gfx::PipelineHandle D3ParticleShading::GetOrBuildPso(const pd3::MaterialDesc& m,
                                                     const D3ParticleFrameInputs& frame) {
    const u64 key = PsoKeyOf(m, frame);
    if (auto it = psos_.find(key); it != psos_.end())
        return it->second;
    auto* gfxDev = rs_.Pipeline().Gfx();
    if (!gfxDev)
        return gfx::PipelineHandle::Invalid;

    // The repacked stream, declared by hand: the element order has to match the
    // VS input struct, because Vulkan, WebGPU and Metal derive shader locations
    // from array position. The two texcoord pairs ride TEXCOORD and TANGENT
    // because slang mangles a NUMBERED semantic on a vertex input — `TEXCOORD1`
    // comes out as index 10 — so every element has to name a digit-free one.
    const gfx::InputElement elements[] = {
        {"POSITION", 0, gfx::Format::R32G32B32_FLOAT, offsetof(D3ParticleVertex, position), 0},
        {"COLOR", 0, gfx::Format::R32G32B32A32_FLOAT, offsetof(D3ParticleVertex, color), 0},
        {"TEXCOORD", 0, gfx::Format::R32G32B32A32_FLOAT, offsetof(D3ParticleVertex, uv01), 0},
        {"TANGENT", 0, gfx::Format::R32G32B32A32_FLOAT, offsetof(D3ParticleVertex, uv23), 0},
        // BLENDWEIGHT for the same reason TANGENT carries uv23: the semantic has
        // to be digit-free, and this is the fifth one the toolchain accepts.
        {"BLENDWEIGHT", 0, gfx::Format::R32_FLOAT, offsetof(D3ParticleVertex, color1), 0},
    };

    gfx::GraphicsPipelineDesc desc{};
    desc.vs = vs_;
    desc.ps = ps_;
    desc.inputLayout = std::span<const gfx::InputElement>(elements);
    desc.inputSlotStrides[0] = sizeof(D3ParticleVertex);
    desc.topology = gfx::PrimitiveTopology::TriangleList;
    desc.blend.enable = m.blendEnable;
    desc.blend.srcColor = D3BlendFactor(m.blendSrc, gfx::BlendFactor::SrcAlpha);
    desc.blend.dstColor = D3BlendFactor(m.blendDst, gfx::BlendFactor::One);
    // A RenderPass carries ONE (src, dst) pair and the engine sets no separate
    // alpha blend, so the two channels take the same enum — but not necessarily
    // the same factor, because factor 11 is the constant 0x00FFFFFF and its
    // alpha is zero where its colour is one.
    desc.blend.srcAlpha = D3BlendFactor(m.blendSrc, gfx::BlendFactor::SrcAlpha, true);
    desc.blend.dstAlpha = D3BlendFactor(m.blendDst, gfx::BlendFactor::One, true);
    // Two flags, not one: 171 of 243 billboard passes leave the alpha masked.
    desc.blend.colorWrite = m.colorWrite;
    desc.blend.alphaWrite = m.alphaWrite;
    // `sub_73DA60` sets ZFUNC and then ZENABLE = (func != Always), so a pass
    // asking for Always is asking for the test to be off — 150 of 1,831 do,
    // including every premultiplied one.
    desc.depthStencil.depthTest = m.depthFunc != 8;
    // Clear on 223 of 223 shipped particle passes, but read rather than
    // assumed: an asset that writes depth is saying its quads are solid.
    desc.depthStencil.depthWrite = m.depthWrite;
    desc.depthStencil.depthCompare = D3CompareOp(m.depthFunc);
    // Cull is a pass property and every particle pass but one asks for none,
    // which is also the only answer a camera-facing quad can take: its winding
    // flips with the camera.
    desc.rasterizer.cull = gfx::CullMode::None;
    desc.rtvFormat = frame.rtvFormat;
    desc.dsvFormat = frame.dsvFormat;
    for (u32 i = 0; i < gfx::GraphicsPipelineDesc::kMaxExtraColorAttachments; ++i)
        desc.extraRtvFormats[i] = frame.extraRtvFormats[i];
    desc.extraRtvCount = frame.extraRtvCount;
    // Writes SV_Target0 alone; the extra attachments are declared only to match
    // the host pass and must keep their cleared contents.
    desc.extraColorWrite = false;

    const gfx::PipelineHandle pso = gfxDev->CreateGraphicsPipeline(desc);
    psos_[key] = pso;
    return pso;
}

bool D3ParticleShading::Draw(gfx::IGFXCommandList* cmd, const particle::EmitterDrawList& dl,
                             const D3ParticleFrameInputs& frame, const model::Actor* owner) {
    if (!cmd || !IsAvailable() || !dl.material.d3)
        return false;
    const pd3::MaterialDesc& m = *dl.material.d3;
    auto* gfxDev = rs_.Pipeline().Gfx();
    if (!gfxDev)
        return false;

    // Without the repacked stream this program has no coordinates to sample at:
    // its whole UV transform lives in the vertex now. Refusing here is what
    // routes the draw to the SD fallback, which samples the diffuse at the raw
    // quad uv — approximate, but not garbage.
    if (!frameReady_ || vb_ == gfx::BufferHandle::Invalid)
        return false;

    const gfx::PipelineHandle pso = GetOrBuildPso(m, frame);
    if (pso == gfx::PipelineHandle::Invalid)
        return false;

    if (auto* c = static_cast<D3ParticleVsCb*>(gfxDev->MapBuffer(vsCb_))) {
        c->view = frame.view.transpose();
        c->projection = frame.projection.transpose();
        c->params = {m.depthBias, 0.0f, 0.0f, 0.0f};
        gfxDev->UnmapBuffer(vsCb_);
    }
    if (auto* c = static_cast<D3ParticlePsCb*>(gfxDev->MapBuffer(psCb_))) {
        for (u32 i = 0; i < pd3::MaterialDesc::kMaxLayers; ++i) {
            f32 uvSet = static_cast<f32>(i);
            f32 colorOp = 0.0f;
            f32 alphaOp = 0.0f;
            Vector4f gain{1.0f, 1.0f, 0.0f, 0.0f};
            if (i < m.layerCount) {
                const pd3::MaterialLayer& L = m.layers[i];
                // The COORDINATE is one of the four the vertex carries, baked by
                // the emitter; which one the pass's texcoord block names, and it
                // is not always this stage's own. The ops and gains stay this
                // stage's -- only the coordinate is routed.
                uvSet = static_cast<f32>(L.uvSet);
                colorOp = static_cast<f32>(L.colorOp);
                alphaOp = static_cast<f32>(L.alphaOp);
                gain = {L.colorGain, L.alphaGain, L.colorClamp ? 1.0f : 0.0f,
                        L.alphaClamp ? 1.0f : 0.0f};
            }
            c->stage[i] = {uvSet, colorOp, alphaOp, 0.0f};
            c->gains[i] = gain;
        }
        // Four bits, because the vertex colour can enter a chain at both ends.
        const f32 vcol = static_cast<f32>((m.colorVcolFirst ? 1 : 0) | (m.colorVcolLast ? 2 : 0) |
                                          (m.alphaVcolFirst ? 4 : 0) | (m.alphaVcolLast ? 8 : 0));
        c->params = {vcol, static_cast<f32>(m.program), m.alphaTest, 0.0f};
        c->params2 = {static_cast<f32>(m.pmaMode), static_cast<f32>(m.alphaFunc),
                      static_cast<f32>(m.flowFirst), static_cast<f32>(m.flowLast)};
        // The literal 10 of `pow(alpha, 10 * COLOR1.a)`; COLOR1.a is per
        // particle and rides the vertex. See MaterialDesc::erosion.
        c->params3 = {m.erosion ? 10.0f : 0.0f, 0.0f, 0.0f, 0.0f};
        gfxDev->UnmapBuffer(psCb_);
    }

    // Pipeline FIRST, then every root binding, and the vertex buffer with them:
    // `BindPipeline` re-sets the root signature on D3D12 and discards constant
    // buffers, textures and samplers bound before it.
    cmd->BindPipeline(pso);
    cmd->BindVertexBuffer(0, vb_, sizeof(D3ParticleVertex));
    cmd->BindConstantBuffer(gfx::ShaderStage::Vertex, 0, vsCb_);
    // Slot 1, not 0: slang assigns registers per MODULE, so the two constant
    // buffers are pinned to b0 and b1 or they collide.
    cmd->BindConstantBuffer(gfx::ShaderStage::Pixel, 1, psCb_);

    // Every declared slot, every draw: a binding Slang kept but the host left
    // unwritten is a Vulkan validation error, not a harmless omission.
    const auto& defaults = rs_.Textures().GetDefaults();
    for (u32 i = 0; i < pd3::MaterialDesc::kMaxLayers; ++i) {
        gfx::TextureHandle tex = gfx::TextureHandle::Invalid;
        u32 wrap = 0x3;
        if (i < m.layerCount) {
            wrap = m.layers[i].wrapFlags;
            if (owner && owner->render.textures && m.layers[i].textureId >= 0)
                tex = owner->render.textures->Get(m.layers[i].textureId);
        }
        // White, not black: an unbound stage multiplies by one, which is what
        // its channel switch already says. A texture that has not landed yet
        // must not turn the whole chain off.
        cmd->BindShaderResource(gfx::ShaderStage::Pixel, i,
                                tex != gfx::TextureHandle::Invalid ? tex : defaults.White);
        cmd->BindSampler(gfx::ShaderStage::Pixel, i, rs_.Samplers().WrapVariant(wrap));
    }

    cmd->Draw(dl.vertexCount, dl.vertexOffset);
    return true;
}

} // namespace whiteout::flakes::renderer::profiles::diablo3
