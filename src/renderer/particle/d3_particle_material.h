#pragma once

// ============================================================================
// d3::MaterialDesc — a Diablo III particle's material, which is not a
// Diablo III *surface's* material.
//
// `Particle_DrawBatch` @0x71000B63A0 resolves the `.prt`'s own `snoShaderMap`
// through the same ShaderMap path geometry uses and then stops: no UberMaterial
// resolve, no Material SNO, no `Render_ResolveMaterialTextureStages`, no
// translucent/opaque pair, and the pass index is the literal 0. What it binds
// instead is four raw texture ids straight off the system record, one per
// STAGE TYPE:
//
//     type  1 <- sys+300      type 19 <- sys+304
//     type 12 <- sys+308      type 14 <- sys+312
//
// and those four are exactly the types the `.prt`'s own `UberMaterial` carries.
// Measured over 21,593 shipped files / 35,631 entries:
//
//     type   1  18,462     type 19  13,157     type 12  2,066     type 14  1,622
//     sets:  {1,19} 10,904   {1} 5,311   none 3,120   {1,12,14,19} 1,431
//
// The other twelve types that appear (25, 16, 40, 42, 58 …, 300 entries
// between them) are declared by no particle pass and are not bound.
//
// The chain that combines them is `Billboard.fx` / `vs_legacy` / `ps_legacy` —
// the engine's FIXED-FUNCTION family — so the per-stage combine block
// io/d3/d3_types.h already decodes for `Legacy.fx` is the authority here too.
// `particle_additive` is one modulate stage; `Particle_transparent_am4x_errosion`
// is three, with `24 24` on the alpha group, which is where the `am4x` in its
// name comes from.
//
// Every layer carries its OWN UV transform, and for particles that is not a
// detail: 9,600 mode-2 entries scale their coordinates (3,372 of them by
// exactly 0.5, 0.5 — a quarter-tile of a sprite sheet) and 13,996 scroll. A
// build that samples all four layers at the quad's own UV draws the whole
// sheet on every particle.
// ============================================================================

#include "io/d3/d3_types.h"
#include "types.h"
#include "whiteout/flakes/types.h"

#include <array>
#include <string>

namespace whiteout::flakes::renderer::particle::d3 {

/// One `MaterialTextureEntry` of a `.prt`, resolved.
struct MaterialLayer {
    /// The `.tex` this layer names. -1 leaves the layer unbound, which draws
    /// as white rather than as nothing — the same answer an unresolved stage
    /// gives in the original.
    i32 textureSno = -1;
    /// Index into the owning actor's texture scope, assigned when the emitter
    /// is created. -1 until then, and for an emitter with no actor.
    i32 textureId = -1;
    /// The entry's `EMaterialTextureType`: 1, 19, 12 or 14. Diagnostics only —
    /// the bind order is this array's order.
    i32 rawType = 0;
    /// From the entry's UV flags word, bits 0 and 1.
    u32 wrapFlags = 0x3;
    /// The entry's own transform, evaluated per frame against the system clock.
    ::whiteout::flakes::io::D3UvXform uv;
    /// @brief Does the pass's combine block sample this stage for the colour /
    ///        the alpha?
    ///
    /// Both default true, because a pass that names no stage is saying "the
    /// program decides" and every particle program that binds a layer samples
    /// it. Where the block does say, it usually says something: type 19 is
    /// alpha-only on 55 of the corpus's 243 billboard passes and both ways on
    /// 95, which is the difference between an alpha mask and a second colour
    /// layer. See D3ResolveParticleMaterial for why the test is "samples"
    /// rather than "modulates".
    bool samplesColor = true;
    bool samplesAlpha = true;
};

/// The whole of a Diablo III particle's material.
struct MaterialDesc {
    /// Four, because four is what `Particle_DrawBatch` binds.
    static constexpr usize kMaxLayers = 4;

    std::array<MaterialLayer, kMaxLayers> layers{};
    /// How many of @ref layers are live, in bind order.
    u32 layerCount = 0;

    /// The `.shm` the pass came from, or -1. Kept for the census.
    i32 snoShaderMap = -1;

    /// @brief Did the ShaderMap chain resolve to a RenderPass?
    ///
    /// False leaves the defaults below standing, which are what the shipped
    /// particle passes overwhelmingly say: cull nothing, write no depth,
    /// blend SrcAlpha/One. Guessing that is better than guessing opaque.
    bool passResolved = false;

    bool blendEnable = true;
    u32 blendSrc = 5; ///< D3DBLEND_SRCALPHA
    u32 blendDst = 2; ///< D3DBLEND_ONE
    bool depthWrite = false;
    /// `RenderPass+60 / 255`. Zero means no alpha test.
    f32 alphaTest = 0.0f;

    /// The stage block's accumulated MODULATE2X / MODULATE4X gains.
    f32 colorGain = 1.0f;
    f32 alphaGain = 1.0f;

    /// `szEffectFile` — `Billboard.fx` for every shipped particle. Carried so
    /// the corpus gate can say so rather than assume it.
    std::string effectFile;

    /// The first layer, which is the diffuse for 18,462 of 18,473 entries that
    /// carry one. -1 when the material binds nothing.
    i32 DiffuseTextureId() const {
        return layerCount > 0 ? layers[0].textureId : -1;
    }
};

} // namespace whiteout::flakes::renderer::particle::d3
