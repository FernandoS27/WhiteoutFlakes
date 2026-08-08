#pragma once

#include <cornflakes/interface/binding/layer_program.hpp>
#include <cornflakes/interface/core/arena.hpp>
#include <cornflakes/interface/diagnostics/issue.hpp>
#include <cornflakes/interface/render/render_packet.hpp>
#include <cornflakes/interface/sim/particle_pool.hpp>

#include <array>
#include <string_view>

namespace whiteout::cornflakes {

struct RenderInputMap {
    std::array<std::string_view, kRenderSlotCount> names{};
};

RenderPacket extractFromPool(const ParticlePool& pool, const LayerProgram& layer, EmitterId emitter,
                             RendererClass cls, const RenderInputMap& mapping, IArena& arena,
                             IssueBag& issues);

RenderInputMap buildRenderInputMapFromAsset(const LayerRenderer& renderer, const LayerProgram& layer);

inline bool hasAssetInputBindings(const LayerRenderer& renderer) noexcept {
    return !renderer.particleInputs.empty();
}

}
