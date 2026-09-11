#pragma once

#include "types.h"
#include "whiteout/flakes/types.h"

#include <memory>

namespace whiteout::flakes::renderer::particle::d3 {
struct MaterialDesc;
}

namespace whiteout::flakes::renderer::particle {

// 8-bit ARGB colour. The vertex stream and the fog combine both work in bytes,
// so lifetime curves evaluate in float and quantise here at the boundary.
struct ImVector {
    u8 a = 0, r = 0, g = 0, b = 0;

    // 0..1 floats, rounded to nearest byte.
    static ImVector FromUnitFloat(f32 rf, f32 gf, f32 bf, f32 af) {
        auto q = [](f32 v) -> u8 {
            f32 x = v * 255.0f + 0.5f;
            if (x <= 0.0f)
                return 0;
            if (x >= 255.0f)
                return 255;
            return static_cast<u8>(x);
        };
        return {q(af), q(rf), q(gf), q(bf)};
    }

    Vector4f ToVec4() const {
        return {r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f};
    }
};

enum class FilterMode : u8 { Blend = 0, Additive = 1, Modulate = 2, Modulate2X = 3, AlphaKey = 4 };

/// Which material program shades a particle draw.
enum class ParticleShading : u8 {
    Bls,          ///< The BLS SD program, off `textureId` and `filterMode`.
    MultiTexture, ///< The three-texture combiner; its offsets index its own stream.
    M3Surface,    ///< A prebuilt SC2 surface, out of the shared particle VB.
    D3,           ///< The Diablo III stage chain.
};

struct ParticleMaterialDesc {
    i32 textureId = -1;
    FilterMode filterMode = FilterMode::Blend;
    bool unshaded = false;
    bool unfogged = false;
    i32 replaceableId = 0;

    /// @brief Shade through the three-texture combiner (M2 `MultiTexture`).
    ///
    /// Set on the DRAW as well as the desc, because it is what tells the
    /// dispatcher that this draw's vertex offsets index the multi-texture
    /// stream rather than the ordinary particle one — the two carry different
    /// vertices and live in different buffers. The two flags below pick among
    /// the client's four shared effects; see M2_MULTITEX_DESIGN.md.
    bool multiTexture = false;
    bool multiTexUse3Colors = false;
    bool multiTexModx4 = false;
    /// Layers 1 and 2, read only when @ref multiTexture is set.
    i32 textureId2 = -1;
    i32 textureId3 = -1;

    /// @brief The Diablo III stage chain, when this draw is one.
    ///
    /// Null for every other dialect, and null is what tells the dispatcher to
    /// shade the draw off @ref textureId and @ref filterMode alone. A `.prt`
    /// binds four textures by stage type with a UV transform each, which the
    /// fields above have nowhere to put; this is an ALIASING pointer into the
    /// emitter's own `d3::EmitterDesc`, so it costs a refcount and never a
    /// copy of the chain. See d3_particle_material.h.
    std::shared_ptr<const d3::MaterialDesc> d3;

    /// @brief The prebuilt SC2 surface this draw shades through, or −1.
    ///
    /// Same shape as the ribbon's `m3Surface` and resolved the same way: the
    /// loader appends one surface per `PAR_` material after the ribbon block
    /// and stamps the index here BEFORE the desc is frozen, so nothing looks a
    /// material up at draw time. −1 leaves the draw on the BLS fallback, which
    /// is also what an unresolved material gets — the pipeline branches on
    /// `>= 0` exactly as `DrawRibbonStrip` does.
    i32 m3Surface = -1;

    /// @brief The program this draw asks for, derived from the fields above.
    ///
    /// Never stored, so it cannot disagree with them, and in the dispatcher's
    /// precedence: the multi-texture stream first, because its vertex offsets
    /// index a different buffer, then a resolved SC2 surface, then the Diablo
    /// III chain; no dialect sets two. A program the renderer cannot run here
    /// — no M3 shading, no D3 program, no owner to look a surface up on — still
    /// falls back to the BLS draw at the call site.
    ParticleShading Shading() const {
        if (multiTexture)
            return ParticleShading::MultiTexture;
        if (m3Surface >= 0)
            return ParticleShading::M3Surface;
        if (d3)
            return ParticleShading::D3;
        return ParticleShading::Bls;
    }
};

} // namespace whiteout::flakes::renderer::particle
