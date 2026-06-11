#pragma once

/// @file
/// @brief Builds a `RenderPacket` from a `ParticlePool` (effect-runtime path, not the medium path).

#include <cornflakes/interface/binding/layer_program.hpp>
#include <cornflakes/interface/core/arena.hpp>
#include <cornflakes/interface/diagnostics/issue.hpp>
#include <cornflakes/interface/render/render_packet.hpp>
#include <cornflakes/interface/sim/particle_pool.hpp>

#include <array>
#include <string_view>

namespace whiteout::cornflakes {

/// @brief Per-slot external-name mapping; empty entries fall back to bind-time defaults.
struct RenderInputMap {
    std::array<std::string_view, kRenderSlotCount> names{};
};

/// @brief Build one packet by reading slot externals out of every harness in `pool`.
RenderPacket extractFromPool(const ParticlePool& pool, const LayerProgram& layer, EmitterId emitter,
                             RendererClass cls, const RenderInputMap& mapping, IArena& arena,
                             IssueBag& issues);

/// @brief Build a renderer's slot→field map from its asset input-pin bindings.
///
/// Translates each `RendererParticleInput` (semantic + indexInStorage) into a `RenderSlot`→field
/// name, resolving the field via `layer.renderFieldNames[indexInStorage]`. This is the engine-exact
/// routing: a multi-renderer layer points each renderer at its own Position/Color/TextureID field,
/// which name-prefix inference cannot recover (all the renderers' Position_<hash> fields share the
/// layer, so inference collapses them to one). Returns an all-empty map if the renderer carries no
/// input bindings (callers fall back to inference).
RenderInputMap buildRenderInputMapFromAsset(const LayerRenderer& renderer, const LayerProgram& layer);

/// @brief True when the renderer carries asset input-pin bindings (prefer over inference).
inline bool hasAssetInputBindings(const LayerRenderer& renderer) noexcept {
    return !renderer.particleInputs.empty();
}

} // namespace whiteout::cornflakes
