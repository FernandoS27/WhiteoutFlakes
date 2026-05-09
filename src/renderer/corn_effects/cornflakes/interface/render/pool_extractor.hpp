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

} // namespace whiteout::cornflakes
