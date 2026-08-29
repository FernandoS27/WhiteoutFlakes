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
};

} // namespace whiteout::flakes::renderer::particle
