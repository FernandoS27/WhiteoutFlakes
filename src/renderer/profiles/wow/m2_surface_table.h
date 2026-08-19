#pragma once

// ============================================================================
// M2SurfaceTable — one entry per `.skin` batch, every combo index resolved.
//
// The format's material binding is five parallel lookup tables reached through
// four *base* indices on the batch: unit `i` reads `combo[base + i]`, not
// `combo[base]`. Chasing that per draw would be four indirections per texture
// unit per frame for data that never changes, so it is resolved once, here.
//
// Nothing animated lives in this table (core/surface_table.h states the split).
// `elementAlpha` and `elementColor` are the load-time constants of a static
// pose; when the animation phase lands they become per-frame values arriving
// through FrameState and this table keeps only their track indices.
// ============================================================================

#include "core/surface_table.h"
#include "core/surface_vocabulary.h"
#include "m2_material.h"
#include "m2_shader_select.h"
#include "whiteout/flakes/types.h"

#include <memory>
#include <vector>

namespace whiteout {
namespace m2 {
struct Model;
}
} // namespace whiteout

namespace whiteout::flakes::renderer::profiles::wow {

/// @brief The most texture units one batch can name.
inline constexpr u32 kM2MaxTextureUnits = 4;

/// @brief One `M2Batch`, flattened for the submission path.
struct M2Surface {
    u16 shaderId = 0;      ///< Raw; drives M2PixelShaderFor / M2VertexShaderFor.
    u8 textureCount = 1;   ///< 1..4.
    M2Blend blend = M2Blend::Opaque;
    u16 materialFlags = 0;
    i32 priorityPlane = 0;
    u16 materialLayer = 0; ///< Capped at 7, as the client does.
    u16 skinSectionIndex = 0;

    /// Renderer texture ids — which are the M2 texture-list indices, because
    /// the adapter emits one TextureData per M2 texture keyed by its index.
    i32 textureId[kM2MaxTextureUnits] = {-1, -1, -1, -1};
    /// -1 environment map, 0 first UV set, 1 second UV set.
    i8 texCoord[kM2MaxTextureUnits] = {0, 0, 0, 0};
    i32 transformId[kM2MaxTextureUnits] = {-1, -1, -1, -1};
    i32 weightId[kM2MaxTextureUnits] = {-1, -1, -1, -1};
    i32 colorIndex = -1;

    /// `batch.color.alpha * batch.textureWeight * model.alpha`, evaluated at
    /// the bind pose.
    f32 elementAlpha = 1.0f;
    Vector3f elementColor = {1.0f, 1.0f, 1.0f};
    /// Per-unit weights, uploaded as one float4.
    f32 unitWeights[kM2MaxTextureUnits] = {1.0f, 1.0f, 1.0f, 1.0f};

    /// `batch.flags & 0x40` — BeginDraw then leaves the texture weight out of
    /// whole-element alpha entirely, and only the shader sees it.
    bool ignoreWeights = false;

    M2PixelShader pixelShader = M2PixelShader::Combiners_Opaque;
    M2VertexShader vertexShader = M2VertexShader::Diffuse_T1;
};

class M2SurfaceTable final : public core::ISurfaceTable {
public:
    static constexpr core::ProductId kProduct = core::ProductId::Wow;

    core::ProductId Product() const override {
        return kProduct;
    }
    usize Count() const override {
        return surfaces_.size();
    }

    /// @brief Surface @p index, or null when out of range — which a draw can
    ///        legitimately hit for an actor mid-reload.
    const M2Surface* Surface(u32 index) const {
        return index < surfaces_.size() ? &surfaces_[index] : nullptr;
    }

    std::vector<M2Surface>& Surfaces() {
        return surfaces_;
    }
    const std::vector<M2Surface>& Surfaces() const {
        return surfaces_;
    }

private:
    std::vector<M2Surface> surfaces_;
};

/// @brief Build the table for skin profile @p profileIndex of @p model.
///        Returns an empty table rather than null when the profile is missing,
///        so callers never branch on a null table.
std::unique_ptr<M2SurfaceTable> BuildM2SurfaceTable(const ::whiteout::m2::Model& model,
                                                    usize profileIndex);

/// @brief BeginDraw's two model-alpha thresholds. Below the first a batch is
///        culled outright; only at or above the second can it take the opaque
///        pass. Between them everything is transparent.
inline constexpr f32 kM2CullModelAlpha = 0.0001f;
inline constexpr f32 kM2OpaqueModelAlpha = 0.99999f;

/// @brief Which bucket a batch draws in, this frame.
///
/// The blend mode does not decide it alone. `CM2Scene::BeginDraw` takes the
/// opaque pass only when the *model* is at full alpha, so a fading creature's
/// Opaque and AlphaKey batches join the sorted transparent set on the way out
/// instead of staying in front of it.
///
/// @param modelAlpha  the model's own alpha — actor visibility times geoset
///                    alpha, NOT the batch's element alpha. The client keeps
///                    the two apart and only this one gates the pass.
core::SurfaceClass M2ClassifySurface(const M2Surface& surface, f32 modelAlpha, f32 elementAlpha);

} // namespace whiteout::flakes::renderer::profiles::wow
